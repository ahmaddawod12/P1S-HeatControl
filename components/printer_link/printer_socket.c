#include "printer_deadline_transport.h"
#include <fcntl.h>
#include <stddef.h>
bool printer_socket_nonblocking_begin(int fd, int *saved_flags) {
  if (fd < 0 || saved_flags == NULL)
    return false;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    return false;
  *saved_flags = flags;
  return true;
}
bool printer_socket_restore(int fd, int saved_flags) {
  return fd >= 0 && fcntl(fd, F_SETFL, saved_flags) == 0;
}
