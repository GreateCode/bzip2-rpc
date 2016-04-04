#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rpc-util.h"

#include <memory>
#include <capnp/ez-rpc.h>

#include "bzlib.capnp.h"

int verbose = 4;  // smaller number => more verbose

static const char *g_exe_file = "./bz2-driver-capnp";
static int g_exe_fd = -1;  // File descriptor to driver executable
/* Before main(), get an FD for the driver program, so that it is still
   accessible even if the application enters a sandbox. */
void __attribute__((constructor)) _stub_construct(void) {
  g_exe_fd = OpenDriver(g_exe_file);
}

class DriverConnection {
public:
  DriverConnection() : pid_(-1), server_address_(nullptr) {
    // Create socket for bootstrap communication with child.
    int socket_fds[2] = {-1, -1};
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, socket_fds);
    if (rc < 0) {
      fatal_("failed to open sockets, errno=%d (%s)", errno, strerror(errno));
    }
    api_("DriverConnection(g_exe_fd=%d, '%s')", g_exe_fd, g_exe_file);

    pid_ = fork();
    if (pid_ < 0) {
      fatal_("failed to fork, errno=%d (%s)", errno, strerror(errno));
    }

    if (pid_ == 0) {
      // Child process: store the socket FD in the environment.
      close(socket_fds[0]);
      char *argv[] = {(char *)"bz2-driver", NULL};
      char nonce_buffer[] = "API_NONCE_FD=xxxxxxxx";
      char * envp[] = {nonce_buffer, NULL};
      sprintf(nonce_buffer, "API_NONCE_FD=%d", socket_fds[1]);
      verbose_("in child process, about to fexecve(fd=%d ('%s'), API_NONCE_FD=%d)", g_exe_fd, g_exe_file, socket_fds[1]);
      // Execute the driver program.
      fexecve(g_exe_fd, argv, envp);
      fatal_("!!! in child process, failed to fexecve, errno=%d (%s)", errno, strerror(errno));
    }

    // Read bootstrap information back from the child:
    // uint32_t len, char server_addr[len]
    uint32_t len;
    rc = read(socket_fds[0], &len, sizeof(len));
    assert (rc == sizeof(len));
    server_address_ = (char *)malloc(len);
    rc = read(socket_fds[0], server_address_, len);
    assert (len == (uint32_t)rc);
    close(socket_fds[0]);
    close(socket_fds[1]);

    client_.reset(new capnp::EzRpcClient(server_address_));
  }

  ~DriverConnection() {
    api_("~DriverConnection({pid=%d})", pid_);
    if (pid_ > 0) {
      int status;
      log_("kill pid_ %d", pid_);
      kill(pid_, SIGKILL);
      log_("reap pid_ %d", pid_);
      pid_t rc = waitpid(pid_, &status, 0);
      log_("reaped pid_ %d, rc=%d, status=%x", pid_, rc, status);
      pid_ = 0;
    }
    if (server_address_) {
      char *filename = server_address_;
      if (strncmp(filename, "unix:", 5) == 0) filename += 5;
      log_("remove socket file '%s'", filename);
      unlink(filename);
      free(server_address_);
    }
  }

  capnp::EzRpcClient* client() {return client_.get();}
  bz2::Bz2::Client cap() {return client_->getMain<bz2::Bz2>();}

private:
  pid_t pid_;  // Child process ID.
  char* server_address_;
  std::unique_ptr<capnp::EzRpcClient> client_;
};

//***************************************************************************
//* Everything above here is generic, and would be useful for any remoted API
//***************************************************************************



// RPC-Forwarding versions of libbz2 entrypoints

extern "C"
int BZ2_bzCompressStream(int ifd, int ofd, int blockSize100k, int verbosity, int workFactor) {
  static const char *method = "BZ2_bzCompressStream";
  DriverConnection conn;
  auto& waitScope = conn.client()->getWaitScope();
  bz2::Bz2::Client cap = conn.cap();
  auto msg = cap.compressStreamRequest();
  msg.setIfd(ifd);
  msg.setOfd(ofd);
  msg.setBlockSize100k(blockSize100k);
  msg.setVerbosity(verbosity);
  msg.setWorkFactor(workFactor);
  api_("%s(%d, %d, %d, %d, %d) =>", method, ifd, ofd, blockSize100k, verbosity, workFactor);
  auto promise = msg.send();
  auto rsp = promise.wait(waitScope);  // blocks till reply arrives
  int retval = rsp.getResult();
  api_("%s(%d, %d, %d, %d, %d) return %d <=", method, ifd, ofd, blockSize100k, verbosity, workFactor, retval);
  return retval;
}

extern "C"
int BZ2_bzDecompressStream(int ifd, int ofd, int verbosity, int small) {
  static const char *method = "BZ2_bzDecompressStream";
  DriverConnection conn;
  auto& waitScope = conn.client()->getWaitScope();
  bz2::Bz2::Client cap = conn.cap();
  auto msg = cap.decompressStreamRequest();
  msg.setIfd(ifd);
  msg.setOfd(ofd);
  msg.setVerbosity(verbosity);
  msg.setSmall(small);
  api_("%s(%d, %d, %d, %d) =>", method, ifd, ofd, verbosity, small);
  auto promise = msg.send();
  auto rsp = promise.wait(waitScope);  // blocks till reply arrives
  int retval = rsp.getResult();
  api_("%s(%d, %d, %d, %d) return %d <=", method, ifd, ofd, verbosity, small, retval);
  return retval;
}

extern "C"
int BZ2_bzTestStream(int ifd, int verbosity, int small) {
  static const char *method = "BZ2_bzTestStream";
  DriverConnection conn;
  auto& waitScope = conn.client()->getWaitScope();
  bz2::Bz2::Client cap = conn.cap();
  auto msg = cap.testStreamRequest();
  msg.setIfd(ifd);
  msg.setVerbosity(verbosity);
  msg.setSmall(small);
  api_("%s(%d, %d, %d) =>", method, ifd, verbosity, small);
  auto promise = msg.send();
  auto rsp = promise.wait(waitScope);  // blocks till reply arrives
  int retval = rsp.getResult();
  api_("%s(%d, %d, %d) return %d <=", method, ifd, verbosity, small, retval);
  return retval;
}

extern "C"
const char *BZ2_bzlibVersion(void) {
  static const char *method = "BZ2_bzlibVersion";
  static const char *saved_version = NULL;
  if (saved_version) {
    api_("%s() return '%s' <= (saved)", method, saved_version);
    return saved_version;
  }
  DriverConnection conn;
  auto& waitScope = conn.client()->getWaitScope();
  bz2::Bz2::Client cap = conn.cap();
  auto msg = cap.libVersionRequest();
  api_("%s() =>", method);
  auto promise = msg.send();
  auto rsp = promise.wait(waitScope);  // blocks till reply arrives
  std::string version = rsp.getVersion();
  api_("%s() return '%s' <=", method, version.c_str());
  saved_version = strdup(version.c_str());
  return saved_version;
}
