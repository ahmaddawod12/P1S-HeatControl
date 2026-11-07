#include "printer_deadline_transport.h"
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>

static int flags = 0x40;
static bool fail_get, fail_set;
int fcntl(int fd, int command, ...) {
    assert(fd == 42);
    if (command == F_GETFL) return fail_get ? -1 : flags;
    assert(command == F_SETFL);
    va_list args;
    va_start(args, command);
    int value = va_arg(args, int);
    va_end(args);
    if (fail_set) return -1;
    flags = value;
    return 0;
}
int main(void) {
    int saved = -1;
    assert(!printer_socket_nonblocking_begin(-1, &saved));
    assert(!printer_socket_nonblocking_begin(42, NULL));
    fail_get = true;
    assert(!printer_socket_nonblocking_begin(42, &saved) && flags == 0x40);
    fail_get = false;
    fail_set = true;
    assert(!printer_socket_nonblocking_begin(42, &saved) && flags == 0x40);
    fail_set = false;
    assert(printer_socket_nonblocking_begin(42, &saved));
    assert(saved == 0x40 && flags == 0x41);
    assert(printer_socket_restore(42, saved) && flags == 0x40);
    flags = 0x41;
    assert(printer_socket_nonblocking_begin(42, &saved));
    assert(printer_socket_restore(42, saved) && flags == 0x41);
    fail_set = true;
    assert(!printer_socket_restore(42, saved));
    assert(!printer_socket_restore(-1, saved));
    puts("Printer socket mode preservation passed");
    return 0;
}
