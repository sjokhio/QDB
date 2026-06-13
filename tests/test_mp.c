/*
 * test_mp.c - minimal multi-process test foundation
 *
 * The executable runs as an orchestrator by default and can also run one
 * worker mode selected through argv. Crash tests are added in later stages.
 *
 * SPDX-License-Identifier: MIT
 */

#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#elif !defined(_CRT_SECURE_NO_WARNINGS)
#  define _CRT_SECURE_NO_WARNINGS
#endif

#include "qdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_platform.h"

#if !defined(_WIN32)
#  include <spawn.h>
#  include <sys/wait.h>
#  include <time.h>

extern char **environ;
#endif

static void sleep_milliseconds(unsigned int milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timespec delay;

    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        /* Resume the remaining delay after a signal. */
    }
#endif
}

static int signal_file(const char *path)
{
    FILE *file = fopen(path, "wb");

    if (!file) {
        return -1;
    }
    return (fclose(file) == 0) ? 0 : -1;
}

static int signal_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (!file) {
        return 0;
    }
    (void)fclose(file);
    return 1;
}

static int wait_for_signal(const char *path)
{
    unsigned int attempts;

    for (attempts = 0; attempts < 500u; ++attempts) {
        if (signal_exists(path)) {
            return 0;
        }
        sleep_milliseconds(10u);
    }
    return -1;
}

static void cleanup_signals(const char *ready_path, const char *release_path)
{
    (void)qdb_test_remove_file(ready_path);
    (void)qdb_test_remove_file(release_path);
}

static int run_open_hold_worker(const char *path, const char *ready_path,
                                const char *release_path)
{
    qdb_t *db = qdb_open(path);
    int rc = EXIT_FAILURE;

    if (!db) {
        return EXIT_FAILURE;
    }
    if (signal_file(ready_path) == 0 && wait_for_signal(release_path) == 0) {
        rc = EXIT_SUCCESS;
    }
    qdb_close(db);
    return rc;
}

static unsigned long current_process_id(void)
{
#if defined(_WIN32)
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

struct worker_process {
#if defined(_WIN32)
    PROCESS_INFORMATION process;
#else
    pid_t pid;
#endif
};

static int start_open_hold_worker(const char *executable, const char *path,
                                  const char *ready_path,
                                  const char *release_path,
                                  struct worker_process *worker)
{
#if defined(_WIN32)
    char command[2048];
    STARTUPINFOA startup;
    int written;
    BOOL created;

    written = snprintf(command, sizeof(command),
                       "\"%s\" --worker=open-hold \"%s\" \"%s\" \"%s\"",
                       executable, path, ready_path, release_path);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return -1;
    }

    memset(&startup, 0, sizeof(startup));
    memset(worker, 0, sizeof(*worker));
    startup.cb = (DWORD)sizeof(startup);

    created = CreateProcessA(executable, command, NULL, NULL, FALSE, 0,
                             NULL, NULL, &startup, &worker->process);
    if (!created) {
        return -1;
    }
    return 0;
#else
    int rc;
    char *const worker_argv[] = {
        (char *)executable,
        (char *)"--worker=open-hold",
        (char *)path,
        (char *)ready_path,
        (char *)release_path,
        NULL
    };

    rc = posix_spawnp(&worker->pid, executable, NULL, NULL,
                      worker_argv, environ);
    return (rc == 0) ? 0 : -1;
#endif
}

static int wait_for_worker(struct worker_process *worker)
{
#if defined(_WIN32)
    DWORD exit_code = 1u;
    int rc = 0;

    if (WaitForSingleObject(worker->process.hProcess, INFINITE) !=
            WAIT_OBJECT_0 ||
        !GetExitCodeProcess(worker->process.hProcess, &exit_code) ||
        exit_code != 0u) {
        rc = -1;
    }

    (void)CloseHandle(worker->process.hThread);
    (void)CloseHandle(worker->process.hProcess);
    return rc;
#else
    int status;
    int rc;

    do {
        rc = waitpid(worker->pid, &status, 0);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        return -1;
    }
    return 0;
#endif
}

static int run_orchestrator(const char *executable)
{
    char db_path[256];
    char ready_path[272];
    char release_path[272];
    struct worker_process worker;
    qdb_t *db;
    int written;
    int rc;
    int failed = 0;

    written = snprintf(db_path, sizeof(db_path), "test_mp_%lu.qdb",
                       current_process_id());
    if (written < 0 || (size_t)written >= sizeof(db_path)) {
        fputs("failed to create temporary database path\n", stderr);
        return EXIT_FAILURE;
    }
    written = snprintf(ready_path, sizeof(ready_path), "%s.ready", db_path);
    if (written < 0 || (size_t)written >= sizeof(ready_path)) {
        fputs("failed to create ready signal path\n", stderr);
        return EXIT_FAILURE;
    }
    written = snprintf(release_path, sizeof(release_path), "%s.release", db_path);
    if (written < 0 || (size_t)written >= sizeof(release_path)) {
        fputs("failed to create release signal path\n", stderr);
        return EXIT_FAILURE;
    }

    qdb_test_cleanup_files(db_path);
    cleanup_signals(ready_path, release_path);
    rc = start_open_hold_worker(executable, db_path, ready_path, release_path,
                                &worker);
    if (rc == 0) {
        int ready_rc = wait_for_signal(ready_path);
        int release_rc = signal_file(release_path);
        int worker_rc = wait_for_worker(&worker);

        rc = (ready_rc == 0 && release_rc == 0 && worker_rc == 0) ? 0 : -1;
    }
    qdb_test_cleanup_files(db_path);
    cleanup_signals(ready_path, release_path);

    if (rc != 0) {
        fputs("worker open-hold mode failed\n", stderr);
        return EXIT_FAILURE;
    }

    puts("QDB multi-process foundation tests");
    puts("  worker open-hold exits successfully                         ok");

    qdb_test_cleanup_files(db_path);
    cleanup_signals(ready_path, release_path);
    rc = start_open_hold_worker(executable, db_path, ready_path, release_path,
                                &worker);
    if (rc != 0) {
        fputs("failed to start lock-hold worker\n", stderr);
        qdb_test_cleanup_files(db_path);
        return EXIT_FAILURE;
    }

    if (wait_for_signal(ready_path) != 0) {
        fputs("lock-hold worker did not signal readiness\n", stderr);
        (void)signal_file(release_path);
        (void)wait_for_worker(&worker);
        qdb_test_cleanup_files(db_path);
        cleanup_signals(ready_path, release_path);
        return EXIT_FAILURE;
    }

    db = qdb_open(db_path);
    if (db != NULL) {
        qdb_close(db);
        failed = 1;
    }

    if (signal_file(release_path) != 0) {
        failed = 1;
    }
    if (wait_for_worker(&worker) != 0) {
        fputs("lock-hold worker failed\n", stderr);
        qdb_test_cleanup_files(db_path);
        cleanup_signals(ready_path, release_path);
        return EXIT_FAILURE;
    }

    db = qdb_open(db_path);
    if (db == NULL) {
        failed = 1;
    } else {
        qdb_close(db);
    }
    qdb_test_cleanup_files(db_path);
    cleanup_signals(ready_path, release_path);

    printf("  concurrent open rejected; reopen after hand-off succeeds   %s\n",
           failed ? "FAILED" : "ok");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--worker=open-hold") == 0) {
        return run_open_hold_worker(argv[2], argv[3], argv[4]);
    }
    if (argc != 1) {
        fprintf(stderr,
                "usage: %s [--worker=open-hold <db_path> <ready> <release>]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    return run_orchestrator(argv[0]);
}
