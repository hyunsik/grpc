/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <forward_list>
#include <functional>
#include <mutex>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include <thread>

#include <gflags/gflags.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc++/async_unary_call.h>
#include <grpc++/config.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/server_credentials.h>
#include <grpc++/status.h>
#include <grpc++/stream.h>
#include <gtest/gtest.h>
#include "src/cpp/server/thread_pool.h"
#include "test/cpp/qps/qpstest.pb.h"
#include "test/cpp/qps/server.h"

#include <grpc/grpc.h>
#include <grpc/support/log.h>

namespace grpc {
namespace testing {

class AsyncQpsServerTest : public Server {
 public:
  AsyncQpsServerTest(const ServerConfig& config, int port)
      : srv_cq_(), async_service_(&srv_cq_), server_(nullptr),
        shutdown_(false) {
    char* server_address = NULL;
    gpr_join_host_port(&server_address, "::", port);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, InsecureServerCredentials());
    gpr_free(server_address);

    builder.RegisterAsyncService(&async_service_);

    server_ = builder.BuildAndStart();

    using namespace std::placeholders;
    request_unary_ = std::bind(&TestService::AsyncService::RequestUnaryCall,
                               &async_service_, _1, _2, _3, &srv_cq_, _4);
    request_streaming_ =
      std::bind(&TestService::AsyncService::RequestStreamingCall,
		&async_service_, _1, _2, &srv_cq_, _3);
    for (int i = 0; i < 100; i++) {
      contexts_.push_front(
          new ServerRpcContextUnaryImpl<SimpleRequest, SimpleResponse>(
              request_unary_, ProcessRPC));
      contexts_.push_front(
          new ServerRpcContextStreamingImpl<SimpleRequest, SimpleResponse>(
              request_streaming_, ProcessRPC));
    }
    for (int i = 0; i < config.threads(); i++) {
      threads_.push_back(std::thread([=]() {
        // Wait until work is available or we are shutting down
        bool ok;
        void* got_tag;
        while (srv_cq_.Next(&got_tag, &ok)) {
	  ServerRpcContext* ctx = detag(got_tag);
	  // The tag is a pointer to an RPC context to invoke
	  if (ctx->RunNextState(ok) == false) {
	    // this RPC context is done, so refresh it
            std::lock_guard<std::mutex> g(shutdown_mutex_);
            if (!shutdown_) {
              ctx->Reset();
            }
	  }
        }
        return;
      }));
    }
  }
  ~AsyncQpsServerTest() {
    server_->Shutdown();
    {
      std::lock_guard<std::mutex> g(shutdown_mutex_);
      shutdown_ = true;
      srv_cq_.Shutdown();
    }
    for (auto thr = threads_.begin(); thr != threads_.end(); thr++) {
      thr->join();
    }
    while (!contexts_.empty()) {
      delete contexts_.front();
      contexts_.pop_front();
    }
  }

 private:
  class ServerRpcContext {
   public:
    ServerRpcContext() {}
    virtual ~ServerRpcContext(){};
    virtual bool RunNextState(bool) = 0;  // next state, return false if done
    virtual void Reset() = 0;         // start this back at a clean state
  };
  static void* tag(ServerRpcContext* func) {
    return reinterpret_cast<void*>(func);
  }
  static ServerRpcContext* detag(void* tag) {
    return reinterpret_cast<ServerRpcContext*>(tag);
  }

  template <class RequestType, class ResponseType>
  class ServerRpcContextUnaryImpl GRPC_FINAL : public ServerRpcContext {
   public:
    ServerRpcContextUnaryImpl(
        std::function<void(ServerContext*, RequestType*,
                           grpc::ServerAsyncResponseWriter<ResponseType>*,
                           void*)> request_method,
        std::function<grpc::Status(const RequestType*, ResponseType*)>
            invoke_method)
        : next_state_(&ServerRpcContextUnaryImpl::invoker),
          request_method_(request_method),
          invoke_method_(invoke_method),
          response_writer_(&srv_ctx_) {
      request_method_(&srv_ctx_, &req_, &response_writer_,
                      AsyncQpsServerTest::tag(this));
    }
    ~ServerRpcContextUnaryImpl() GRPC_OVERRIDE {}
    bool RunNextState(bool ok) GRPC_OVERRIDE {return (this->*next_state_)(ok);}
    void Reset() GRPC_OVERRIDE {
      srv_ctx_ = ServerContext();
      req_ = RequestType();
      response_writer_ =
          grpc::ServerAsyncResponseWriter<ResponseType>(&srv_ctx_);

      // Then request the method
      next_state_ = &ServerRpcContextUnaryImpl::invoker;
      request_method_(&srv_ctx_, &req_, &response_writer_,
                      AsyncQpsServerTest::tag(this));
    }

   private:
    bool finisher(bool) { return false; }
    bool invoker(bool ok) {
      if (!ok)
	return false;

      ResponseType response;

      // Call the RPC processing function
      grpc::Status status = invoke_method_(&req_, &response);

      // Have the response writer work and invoke on_finish when done
      next_state_ = &ServerRpcContextUnaryImpl::finisher;
      response_writer_.Finish(response, status, AsyncQpsServerTest::tag(this));
      return true;
    }
    ServerContext srv_ctx_;
    RequestType req_;
    bool (ServerRpcContextUnaryImpl::*next_state_)(bool);
    std::function<void(ServerContext*, RequestType*,
                       grpc::ServerAsyncResponseWriter<ResponseType>*, void*)>
        request_method_;
    std::function<grpc::Status(const RequestType*, ResponseType*)>
        invoke_method_;
    grpc::ServerAsyncResponseWriter<ResponseType> response_writer_;
  };

  template <class RequestType, class ResponseType>
  class ServerRpcContextStreamingImpl GRPC_FINAL : public ServerRpcContext {
   public:
    ServerRpcContextStreamingImpl(
        std::function<void(ServerContext *,
                           grpc::ServerAsyncReaderWriter<ResponseType,
			   RequestType> *, void *)> request_method,
        std::function<grpc::Status(const RequestType *, ResponseType *)>
            invoke_method)
        : next_state_(&ServerRpcContextStreamingImpl::request_done),
          request_method_(request_method),
          invoke_method_(invoke_method),
          stream_(&srv_ctx_) {
      request_method_(&srv_ctx_, &stream_, AsyncQpsServerTest::tag(this));
    }
    ~ServerRpcContextStreamingImpl() GRPC_OVERRIDE {
    }
    bool RunNextState(bool ok) GRPC_OVERRIDE {return (this->*next_state_)(ok);}
    void Reset() GRPC_OVERRIDE {
      srv_ctx_ = ServerContext();
      req_ = RequestType();
      stream_ = grpc::ServerAsyncReaderWriter<ResponseType,
					      RequestType>(&srv_ctx_);

      // Then request the method
      next_state_ = &ServerRpcContextStreamingImpl::request_done;
      request_method_(&srv_ctx_, &stream_, AsyncQpsServerTest::tag(this));
    }

   private:
    bool request_done(bool ok) {
      if (!ok)
	return false;
      stream_.Read(&req_, AsyncQpsServerTest::tag(this));
      next_state_ = &ServerRpcContextStreamingImpl::read_done;
      return true;
    }

    bool read_done(bool ok) {
      if (ok) {
	// invoke the method
	ResponseType response;
	// Call the RPC processing function
	grpc::Status status = invoke_method_(&req_, &response);
	// initiate the write
	stream_.Write(response, AsyncQpsServerTest::tag(this));
	next_state_ = &ServerRpcContextStreamingImpl::write_done;
      } else {	// client has sent writes done
	// finish the stream
	stream_.Finish(Status::OK, AsyncQpsServerTest::tag(this));
	next_state_ = &ServerRpcContextStreamingImpl::finish_done;
      }
      return true;
    }
    bool write_done(bool ok) {
      // now go back and get another streaming read!
      if (ok) {
	stream_.Read(&req_, AsyncQpsServerTest::tag(this));
	next_state_ = &ServerRpcContextStreamingImpl::read_done;
      }
      else {
	stream_.Finish(Status::OK, AsyncQpsServerTest::tag(this));
	next_state_ = &ServerRpcContextStreamingImpl::finish_done;
      }
      return true;
    }
    bool finish_done(bool ok) {return false; /* reset the context */ }

    ServerContext srv_ctx_;
    RequestType req_;
    bool (ServerRpcContextStreamingImpl::*next_state_)(bool);
    std::function<void(ServerContext *,
		       grpc::ServerAsyncReaderWriter<ResponseType,
		       RequestType> *, void *)> request_method_;
    std::function<grpc::Status(const RequestType *, ResponseType *)>
        invoke_method_;
    grpc::ServerAsyncReaderWriter<ResponseType,RequestType> stream_;
  };

  static Status ProcessRPC(const SimpleRequest* request,
			   SimpleResponse* response) {
    if (request->response_size() > 0) {
      if (!SetPayload(request->response_type(), request->response_size(),
                      response->mutable_payload())) {
        return Status(grpc::StatusCode::INTERNAL, "Error creating payload.");
      }
    }
    return Status::OK;
  }
  CompletionQueue srv_cq_;
  TestService::AsyncService async_service_;
  std::vector<std::thread> threads_;
  std::unique_ptr<grpc::Server> server_;
  std::function<void(ServerContext*, SimpleRequest*,
                     grpc::ServerAsyncResponseWriter<SimpleResponse>*, void*)>
      request_unary_;
  std::function<void(ServerContext*, grpc::ServerAsyncReaderWriter<
		     SimpleResponse,SimpleRequest>*, void*)>
      request_streaming_;
  std::forward_list<ServerRpcContext*> contexts_;

  std::mutex shutdown_mutex_;
  bool shutdown_;
};

std::unique_ptr<Server> CreateAsyncServer(const ServerConfig& config,
                                          int port) {
  return std::unique_ptr<Server>(new AsyncQpsServerTest(config, port));
}

}  // namespace testing
}  // namespace grpc
