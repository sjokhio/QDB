/*
 * hello.c — minimal QDB usage example (placeholder)
 *
 * This example will demonstrate the basic push / pop / ack cycle once
 * the storage engine is implemented.  For now it prints the library
 * version and exercises the error-message utility.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    printf("QDB version %s\n", qdb_version());

    /*
     * TODO: replace with real usage once qdb_pop is implemented:
     *
     *   qdb_t *db = qdb_open("hello.qdb");
     *   qdb_push(db, "greetings", "hello", 5);
     *
     *   qdb_msg_t msg = {0};
     *   if (qdb_pop(db, "greetings", &msg) == QDB_OK) {
     *       printf("got: %.*s\n", (int)msg.len, (const char *)msg.data);
     *       qdb_ack(db, msg.id);
     *       qdb_msg_free(&msg);
     *   }
     *
     *   qdb_close(db);
     */

    printf("qdb_pop not yet implemented.\n");
    printf("Error codes:\n");

    int codes[] = {
        QDB_OK, QDB_ERR_IO, QDB_ERR_CORRUPT, QDB_ERR_INVAL,
        QDB_ERR_EMPTY, QDB_ERR_NOENT, QDB_ERR_NOMEM, QDB_ERR_LOCKED,
    };

    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        printf("  %4d  %s\n", codes[i], qdb_errmsg(codes[i]));
    }

    return EXIT_SUCCESS;
}
