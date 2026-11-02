#include "printer_deadline_transport.h"
#include <fcntl.h>
bool printer_socket_nonblocking(int fd) {
  if (fd < 0)
    return false;
  int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
