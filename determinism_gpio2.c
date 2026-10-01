#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <gpiod.h>

#define CONSUMER    "determinism-test"
#define RT_PRIORITY 80

/* Kullanim: sudo ./determinism_gpio2 <gpiochip> <rt_line> <normal_line> <periyot_us> <sure_s>
 * Ornek:    sudo ./determinism_gpio2 gpiochip0 15 16 500 30
 *
 * Iki ayri is parcacigi, ayni anda, ayni nominal periyotla iki farkli GPIO'yu toggle eder:
 *   - RT kanali   (rt_line):  SCHED_FIFO oncelik 80 + clock_nanosleep MUTLAK zaman.
 *                             Surukleme birikmez, Preempt-RT'nin garanti ettigi sey budur.
 *   - Normal kanal (normal_line): SCHED_OTHER (varsayilan) + usleep GORECELI zaman.
 *                             Sistem me\u015fgulken zamanlayici bu is parcacigini rahatlikla
 *                             oteler, usleep de gecen suredeki gecikmeyi telafi etmez ->
 *                             yuk altinda surukleme/jitter BIRIKIR.
 *
 * Lojik analizorde iki kanali yan yana izleyince: RT kanali her yukte de duzgun kare dalga
 * kalmali, normal kanal ise stress-ng calisirken gorunur sekilde bozulmali/kaymali. Bu ikisi
 * arasindaki FARK, Preempt-RT'nin sagladigi gercek, olculebilir determinizm kanitidir.
 */

struct thread_arg {
    struct gpiod_chip *chip;
    unsigned int line_offset;
    long period_us;
    int duration_s;
    int use_rt;        /* 1: SCHED_FIFO + mutlak zaman, 0: SCHED_OTHER + goreceli usleep */
    const char *label;
};

static void *toggle_thread(void *arg_ptr)
{
    struct thread_arg *arg = (struct thread_arg *)arg_ptr;
    long period_ns = arg->period_us * 1000L;

    struct gpiod_line *line = gpiod_chip_get_line(arg->chip, arg->line_offset);
    if (!line) {
        perror("gpiod_chip_get_line");
        return NULL;
    }
    if (gpiod_line_request_output(line, CONSUMER, 0) < 0) {
        perror("gpiod_line_request_output");
        return NULL;
    }

    if (arg->use_rt) {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = RT_PRIORITY;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            perror("pthread_setschedparam (sudo ile calistirdin mi?)");
            return NULL;
        }
    }
    /* use_rt == 0 ise: hicbir sey yapma, varsayilan SCHED_OTHER'da kal */

    struct timespec next, now, deadline;
    clock_gettime(CLOCK_MONOTONIC, &next);
    deadline = next;
    deadline.tv_sec += arg->duration_s;

    long min_jitter_ns = 1000000000L, max_jitter_ns = 0, sum_jitter_ns = 0;
    long count = 0;
    int value = 0;

    for (;;) {
        if (arg->use_rt) {
            /* Mutlak hedef zaman: onceki gecikme bir sonrakine tasinmaz */
            next.tv_nsec += period_ns;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec++;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        } else {
            /* Ideal periyodik hedefi RT ile AYNI mantikla ilerlet (next += period_ns),
             * ama sadece GORECELI usleep ile uyu ve duzeltme yapma. Boylece "next" hep
             * "mukemmel periyodik sinyal simdi burada olurdu" hedefini tutar, "now" ise
             * gercekte nerede oldugumuzu gosterir -- ikisi arasindaki fark GERCEK, birikmis
             * surukleme/jitter'dir (RT kanaliyla birebir karsilastirilabilir bir metrik). */
            next.tv_nsec += period_ns;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec++;
            }
            usleep((useconds_t)arg->period_us);
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        long jitter_ns = (now.tv_sec - next.tv_sec) * 1000000000L
                        + (now.tv_nsec - next.tv_nsec);
        if (jitter_ns < 0) jitter_ns = 0;
        if (jitter_ns > max_jitter_ns) max_jitter_ns = jitter_ns;
        if (jitter_ns < min_jitter_ns) min_jitter_ns = jitter_ns;
        sum_jitter_ns += jitter_ns;
        count++;

        value = !value;
        gpiod_line_set_value(line, value);

        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec > deadline.tv_nsec)) {
            break;
        }
    }

    printf("\n--- %s sonuclari ---\n", arg->label);
    printf("Toplam toggle sayisi : %ld\n", count);
    printf("Min jitter            : %ld ns\n", min_jitter_ns);
    printf("Max jitter            : %ld ns\n", max_jitter_ns);
    printf("Ortalama jitter       : %ld ns\n", count ? sum_jitter_ns / count : 0);

    gpiod_line_release(line);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "Kullanim: %s <gpiochip> <rt_line> <normal_line> <periyot_us> <sure_s>\n", argv[0]);
        fprintf(stderr, "Ornek:    %s gpiochip0 15 16 500 30\n", argv[0]);
        return 1;
    }

    const char *chip_name = argv[1];
    unsigned int rt_line     = (unsigned int)strtoul(argv[2], NULL, 10);
    unsigned int normal_line = (unsigned int)strtoul(argv[3], NULL, 10);
    long period_us            = strtol(argv[4], NULL, 10);
    int duration_s           = atoi(argv[5]);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        perror("mlockall (sudo ile calistirdin mi?)");
        return 1;
    }

    struct gpiod_chip *chip = gpiod_chip_open_by_name(chip_name);
    if (!chip) {
        perror("gpiod_chip_open_by_name");
        return 1;
    }

    printf("Baslatiliyor: %s\n", chip_name);
    printf("  RT kanal     : line %u (SCHED_FIFO %d, mutlak zaman)\n", rt_line, RT_PRIORITY);
    printf("  Normal kanal : line %u (SCHED_OTHER, goreceli usleep)\n", normal_line);
    printf("  Periyot: %ld us, sure: %d s\n", period_us, duration_s);
    printf("Lojik analizoru simdi iki pine de bagla ve yakalamayi baslat.\n\n");
    sleep(2);

    struct thread_arg rt_arg = {
        .chip = chip, .line_offset = rt_line, .period_us = period_us,
        .duration_s = duration_s, .use_rt = 1, .label = "RT kanal (SCHED_FIFO)"
    };
    struct thread_arg normal_arg = {
        .chip = chip, .line_offset = normal_line, .period_us = period_us,
        .duration_s = duration_s, .use_rt = 0, .label = "Normal kanal (SCHED_OTHER)"
    };

    pthread_t rt_tid, normal_tid;
    if (pthread_create(&rt_tid, NULL, toggle_thread, &rt_arg) != 0) {
        perror("pthread_create (rt)");
        return 1;
    }
    if (pthread_create(&normal_tid, NULL, toggle_thread, &normal_arg) != 0) {
        perror("pthread_create (normal)");
        return 1;
    }

    pthread_join(rt_tid, NULL);
    pthread_join(normal_tid, NULL);

    printf("\nSimdi lojik analizor yakalamasini durdur ve iki kanalin periyot/jitter\n");
    printf("degerlerini gorsel olarak karsilastir. Ikinci calistirmayi stress-ng ile\n");
    printf("sistemi yukleyerek tekrarla; RT kanalin duzgun kalmasi, normal kanalin\n");
    printf("bozulmasi beklenen sonuctur.\n");

    gpiod_chip_close(chip);
    return 0;
}
