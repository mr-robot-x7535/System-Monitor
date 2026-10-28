#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

typedef struct {
    unsigned long long total;
    unsigned long long idle;
} CpuStats;

typedef struct {
    unsigned long long total;
    unsigned long long available;
} MemoryStats;

static volatile sig_atomic_t running = 1;

void handle_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

int read_cpu(CpuStats *stats) {
    FILE *file = fopen("/proc/stat", "r");
    if (!file) return 0;

    char line[512];
    unsigned long long user, nice, system, idle, iowait;
    unsigned long long irq, softirq, steal;

    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return 0;
    }

    fclose(file);

    int count = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

    if (count < 4) return 0;

    stats->idle = idle + (count >= 5 ? iowait : 0);
    stats->total = user + nice + system + idle;

    if (count >= 5) stats->total += iowait;
    if (count >= 6) stats->total += irq;
    if (count >= 7) stats->total += softirq;
    if (count >= 8) stats->total += steal;

    return 1;
}

int read_memory(MemoryStats *stats) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) return 0;

    char line[256];
    unsigned long long total = 0;
    unsigned long long available = 0;

    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemTotal: %llu kB", &total) == 1) continue;
        if (sscanf(line, "MemAvailable: %llu kB", &available) == 1) continue;
    }

    fclose(file);

    if (total == 0) return 0;

    stats->total = total;
    stats->available = available;

    return 1;
}

double calculate_cpu(CpuStats previous, CpuStats current) {
    unsigned long long total_delta = current.total - previous.total;
    unsigned long long idle_delta = current.idle - previous.idle;

    if (total_delta == 0 || idle_delta > total_delta) return 0.0;

    return 100.0 * (double)(total_delta - idle_delta) / total_delta;
}

void format_uptime(unsigned long long seconds, char *buffer, size_t size) {
    unsigned long long days = seconds / 86400;
    unsigned long long hours = (seconds % 86400) / 3600;
    unsigned long long minutes = (seconds % 3600) / 60;
    unsigned long long secs = seconds % 60;

    snprintf(buffer, size, "%llud %02lluh %02llum %02llus",
        days, hours, minutes, secs);
}

unsigned long long read_uptime(void) {
    FILE *file = fopen("/proc/uptime", "r");
    if (!file) return 0;

    double seconds = 0.0;
    fscanf(file, "%lf", &seconds);
    fclose(file);

    return seconds >= 0.0 ? (unsigned long long)seconds : 0;
}

int main(int argc, char **argv) {
    unsigned long interval = 2;
    double cpu_limit = 90.0;
    double memory_limit = 90.0;
    int log_enabled = 1;

    if (argc > 1) {
        char *end;
        unsigned long value = strtoul(argv[1], &end, 10);
        if (*argv[1] && *end == '\0' && value >= 1 && value <= 3600)
            interval = value;
        else {
            fprintf(stderr, "Intervalo invalido.\n");
            return 1;
        }
    }

    if (argc > 2) {
        char *end;
        double value = strtod(argv[2], &end);
        if (*argv[2] && *end == '\0' && value >= 1.0 && value <= 100.0)
            cpu_limit = value;
        else {
            fprintf(stderr, "Limite de CPU invalido.\n");
            return 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    FILE *log = NULL;

    if (log_enabled) {
        log = fopen("megabrain_monitor.csv", "a");
        if (!log) {
            perror("Nao foi possivel abrir o historico");
            return 1;
        }

        if (ftell(log) == 0)
            fprintf(log, "data_hora,cpu_percentual,ram_percentual,ram_usada_mb,ram_total_mb\n");
    }

    CpuStats previous;

    if (!read_cpu(&previous)) {
        fprintf(stderr, "Falha ao ler estatisticas da CPU.\n");
        if (log) fclose(log);
        return 1;
    }

    printf("MegaBrain C - Monitor de sistema\n");
    printf("Intervalo: %lu segundo(s)\n", interval);
    printf("Limite de CPU: %.1f%%\n", cpu_limit);
    printf("Pressione Ctrl+C para encerrar.\n");

    while (running) {
        sleep(interval);

        if (!running) break;

        CpuStats current;
        MemoryStats memory;

        if (!read_cpu(&current) || !read_memory(&memory)) {
            fprintf(stderr, "Falha ao coletar metricas.\n");
            continue;
        }

        double cpu = calculate_cpu(previous, current);
        previous = current;

        unsigned long long used = memory.total - memory.available;
        double ram = 100.0 * (double)used / (double)memory.total;

        time_t now = time(NULL);
        struct tm local_time;
        char timestamp[32];
        char uptime[64];

        localtime_r(&now, &local_time);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
        format_uptime(read_uptime(), uptime, sizeof(uptime));

        printf("\033[2J\033[H");
        printf("========================================\n");
        printf("         MEGABRAIN SYSTEM MONITOR\n");
        printf("========================================\n");
        printf("Data:       %s\n", timestamp);
        printf("Uptime:     %s\n", uptime);
        printf("----------------------------------------\n");
        printf("CPU:        %6.2f%%\n", cpu);
        printf("RAM usada:  %6.2f%%\n", ram);
        printf("RAM usada:  %llu MB / %llu MB\n",
            used / 1024, memory.total / 1024);
        printf("----------------------------------------\n");

        if (cpu >= cpu_limit)
            printf("ALERTA: consumo elevado de CPU!\n");

        if (ram >= memory_limit)
            printf("ALERTA: consumo elevado de RAM!\n");

        printf("========================================\n");
        fflush(stdout);

        if (log) {
            fprintf(log, "%s,%.2f,%.2f,%llu,%llu\n",
                timestamp, cpu, ram, used / 1024, memory.total / 1024);
            fflush(log);
        }
    }

    if (log) fclose(log);

    printf("\nMonitor encerrado. Historico salvo em CSV.\n");

    return 0;
}