/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "WatchmanConnection.h"

#include <folly/ExceptionWrapper.h>
#include <folly/SocketAddress.h>
#include <folly/Subprocess.h>
#include <folly/experimental/bser/Bser.h>
#include <folly/futures/InlineExecutor.h>

namespace watchman {

using namespace folly::bser;
using namespace folly;

// Ordered with the most likely kind first
static const std::vector<dynamic> kUnilateralLabels{"subscription", "log"};

static const dynamic kError("error");
static const dynamic kCapabilities("capabilities");

// We'll just dispatch bser decodes and callbacks inline unless they
// give us an alternative environment
static InlineExecutor inlineExecutor;

WatchmanConnection::WatchmanConnection(
    EventBase* eventBase,
    Optional<std::string>&& sockPath,
    Optional<WatchmanConnection::Callback>&& callback,
    Executor* cpuExecutor)
    : eventBase_(eventBase),
      sockPath_(std::move(sockPath)),
      callback_(std::move(callback)),
      cpuExecutor_(cpuExecutor ? cpuExecutor : &inlineExecutor),
      versionCmd_(nullptr),
      bufQ_(IOBufQueue::cacheChainLength()) {
  CHECK_NOTNULL(eventBase);
}

WatchmanConnection::~WatchmanConnection() {
  close();
  // If there are outstanding references to 'this' in callbacks they are about
  // to become invalid. The correct way to avoid this is to close() the
  // WatchmanConnection explicitly and then flush the event base for the
  // cpuExecutor_ before allowing destruction.
  CHECK_EQ(destructorGuardRefCount_, 0);
}

folly::Future<std::string> WatchmanConnection::getSockPath() {
  // Take explicit configuration first
  if (sockPath_.hasValue()) {
    return makeFuture(sockPath_.value());
  }

  // Else use the environmental variable used by watchman to report
  // the active socket path
  auto var = getenv("WATCHMAN_SOCK");
  if (var && *var) {
    return makeFuture(std::string(var));
  }

  return via(cpuExecutor_, [] {
    // Else discover it from the CLI
    folly::Subprocess proc(
        {"watchman", "--output-encoding=bser", "get-sockname"},
        folly::Subprocess::pipeStdout() |
            folly::Subprocess::pipeStderr().usePath());
    SCOPE_FAIL {
      // Always clean up to avoid Subprocess asserting on destruction
      proc.kill();
      proc.wait();
    };
    auto out_pair = proc.communicate();
    auto result = parseBser(out_pair.first);
    proc.waitChecked();
    return result["sockname"].asString();
  });
}

Future<dynamic> WatchmanConnection::connect(folly::dynamic versionArgs) {
  if (!versionArgs.isObject()) {
    throw WatchmanError("versionArgs must be object");
  }
  versionCmd_ = folly::dynamic::array("version", versionArgs);

  WatchmanConnectionGuard guard(*this);
  auto res = getSockPath().then([this, guard](std::string&& path) {
    eventBase_->runInEventBaseThread([=] {
      folly::SocketAddress addr;
      addr.setFromPath(path);

      sock_ = folly::AsyncSocket::newSocket(eventBase_);
      sock_->connect(this, addr);
    });

    return connectPromise_.getFuture();
  });
  return res;
}

void WatchmanConnection::close() {
  if (closing_) {
    return;
  }
  closing_ = true;
  if (sock_) {
    eventBase_->runImmediatelyOrRunInEventBaseThreadAndWait([this] {
      sock_->close();
      sock_.reset();
    });
  }
  failQueuedCommands(
      make_exception_wrapper<WatchmanError>(
          "WatchmanConnection::close() was called"));
}

// The convention for Watchman responses is that they represent
// an error if they contain the "error" key.  We want to report
// those as exceptions, but it is easier to do that via a Try
Try<dynamic> WatchmanConnection::watchmanResponseToTry(dynamic&& value) {
  auto error = value.get_ptr(kError);
  if (error) {
    return Try<dynamic>(make_exception_wrapper<WatchmanResponseError>(value));
  }
  return Try<dynamic>(std::move(value));
}

void WatchmanConnection::connectSuccess() noexcept {
  try {
    sock_->setReadCB(this);
    sock_->setCloseOnExec();

    WatchmanConnectionGuard guard(*this);
    run(versionCmd_).then([this, guard](dynamic&& result) {
      // If there is no "capabilities" key then the version of
      // watchman is too old; treat this as an error
      if (!result.get_ptr(kCapabilities)) {
        result["error"] =
            "This watchman server has no support for capabilities, "
            "please upgrade to the current stable version of watchman";
        connectPromise_.setTry(watchmanResponseToTry(std::move(result)));
        return;
      }
      connectPromise_.setValue(std::move(result));
    }).onError([this, guard](const folly::exception_wrapper& e) {
      connectPromise_.setException(e);
    });
  } catch(const std::exception& e) {
    connectPromise_.setException(
      folly::exception_wrapper(std::current_exception(), e));
  } catch(...) {
    connectPromise_.setException(
      folly::exception_wrapper(std::current_exception()));
  }
}

void WatchmanConnection::connectErr(
    const folly::AsyncSocketException& ex) noexcept {
  connectPromise_.setException(ex);
}

WatchmanConnection::QueuedCommand::QueuedCommand(const dynamic& command)
    : cmd(command) {}

Future<dynamic> WatchmanConnection::run(const dynamic& command) noexcept {
  auto cmd = std::make_shared<QueuedCommand>(command);
  if (broken_) {
    cmd->promise.setException(WatchmanError("The connection was broken"));
    return cmd->promise.getFuture();
  }
  if (!sock_) {
    cmd->promise.setException(WatchmanError(
        "No socket (did you call connect() and check result for exceptions?)"));
    return cmd->promise.getFuture();
  }

  bool shouldWrite;
  {
    std::lock_guard<std::mutex> g(mutex_);
    // We only need to call sendCommand if we don't have a command in
    // progress; the completion handler will trigger it once we receive
    // the response
    shouldWrite = commandQ_.empty();
    commandQ_.push_back(cmd);
  }

  if (shouldWrite) {
    WatchmanConnectionGuard guard(*this);
    eventBase_->runInEventBaseThread([this, guard] { sendCommand(); });
  }

  return cmd->promise.getFuture();
}

// Generate a failure for all queued commands
void WatchmanConnection::failQueuedCommands(
    const folly::exception_wrapper& ex) {
  std::lock_guard<std::mutex> g(mutex_);
  auto q = commandQ_;
  commandQ_.clear();

  broken_ = true;
  for (auto& cmd : q) {
    if (!cmd->promise.isFulfilled()) {
      cmd->promise.setException(ex);
    }
  }

  // If the user has explicitly closed the connection no need for callback
  if (callback_ && !closing_) {
    WatchmanConnectionGuard guard(*this);
    cpuExecutor_->add([this, guard, ex] {
      (*callback_)(folly::Try<folly::dynamic>(ex));
    });
  }
}

// Sends the next eligible command to the Watchman service
void WatchmanConnection::sendCommand(bool pop) {
  std::shared_ptr<QueuedCommand> cmd;

  {
    std::lock_guard<std::mutex> g(mutex_);

    if (pop) {
      // We finished processing this one, discard it and focus
      // on the next item, if any.
      commandQ_.pop_front();
    }
    if (commandQ_.empty()) {
      return;
    }
    cmd = commandQ_.front();
  }

  sock_->writeChain(this, toBserIOBuf(cmd->cmd, serialization_opts()));
}

void WatchmanConnection::popAndSendCommand() {
  sendCommand(/* pop = */ true);
}

// Called when AsyncSocket::writeChain completes
void WatchmanConnection::writeSuccess() noexcept {
  // Don't care particularly
}

// Called when AsyncSocket::writeChain fails
void WatchmanConnection::writeErr(
    size_t,
    const folly::AsyncSocketException& ex) noexcept {
  failQueuedCommands(ex);
}

// Called when AsyncSocket wants to give us data
void WatchmanConnection::getReadBuffer(void** bufReturn, size_t* lenReturn) {
  std::lock_guard<std::mutex> g(mutex_);
  const auto ret = bufQ_.preallocate(2048, 2048);
  *bufReturn = ret.first;
  *lenReturn = ret.second;
}

// Called when AsyncSocket gave us data
void WatchmanConnection::readDataAvailable(size_t len) noexcept {
  {
    std::lock_guard<std::mutex> g(mutex_);
    bufQ_.postallocate(len);
  }
  WatchmanConnectionGuard guard(*this);
  cpuExecutor_->add([this, guard] { decodeNextResponse(); });
}

std::unique_ptr<folly::IOBuf> WatchmanConnection::splitNextPdu() {
  std::lock_guard<std::mutex> g(mutex_);
  if (!bufQ_.front()) {
    return nullptr;
  }

  // Do we have enough data to decode the next item?
  size_t pdu_len = 0;
  try {
    pdu_len = decodePduLength(bufQ_.front());
  } catch (const std::out_of_range&) {
    // Don't have enough data yet
    return nullptr;
  }

  if (pdu_len > bufQ_.chainLength()) {
    // Don't have enough data yet
    return nullptr;
  }

  // Remove the PDU blob from the front of the chain
  return bufQ_.split(pdu_len);
}

// Try to peel off one or more PDU's from our buffer queue.
// Decode each complete PDU from BSER -> dynamic and dispatch
// either the associated QueuedCommand or to the callback_ for
// unilateral responses.
// This is executed via the cpuExecutor.  We only allow one
// thread to carry out the decoding at a time so that the callbacks
// are triggered in the order that they are received.  It is possible
// for us to receive a large PDU followed by a small one and for the
// small one to finish decoding before the large one, so we must
// serialize the dispatching.
void WatchmanConnection::decodeNextResponse() {
  {
    std::lock_guard<std::mutex> g(mutex_);
    if (decoding_) {
      return;
    }
    decoding_ = true;
  }

  SCOPE_EXIT {
    std::lock_guard<std::mutex> g(mutex_);
    decoding_ = false;
  };

  while (true) {
    auto pdu = splitNextPdu();
    if (!pdu) {
      return;
    }

    try {
      auto decoded = parseBser(pdu.get());

      bool is_unilateral = false;
      // Check for a unilateral response
      for (const auto& k : kUnilateralLabels) {
        if (decoded.get_ptr(k)) {
          // This is a unilateral response
          if (callback_.hasValue()) {
            callback_.value()(watchmanResponseToTry(std::move(decoded)));
            is_unilateral = true;
            break;
          }
          // No callback; usage error :-/
          failQueuedCommands(
              std::runtime_error("No unilateral callback has been installed"));
          return;
        }
      }
      if (is_unilateral) {
        continue;
      }

      // It's actually a command response; get the cmd so that we
      // can fulfil its promise
      std::shared_ptr<QueuedCommand> cmd;
      {
        std::lock_guard<std::mutex> g(mutex_);
        if (commandQ_.empty()) {
          failQueuedCommands(
              std::runtime_error("No commands have been queued"));
          return;
        }
        cmd = commandQ_.front();
      }

      // Dispatch outside of the lock in case it tries to send another
      // command
      cmd->promise.setTry(watchmanResponseToTry(std::move(decoded)));

      // Now we're in a position to send the next queued command.
      // We remove it after dispatching the try above in case that
      // queued up more commands; we want to be the one thing that
      // is responsible for sending the next queued command here
      popAndSendCommand();
    } catch (const std::exception& ex) {
      failQueuedCommands(ex);
      return;
    }
  }
}

// Called when AsyncSocket hits EOF
void WatchmanConnection::readEOF() noexcept {
  failQueuedCommands(
      std::system_error(ENOTCONN, std::system_category(), "connection closed"));
}

// Called when AsyncSocket has a read error
void WatchmanConnection::readErr(
    const folly::AsyncSocketException& ex) noexcept {
  failQueuedCommands(ex);
}
} // namespace watchman
