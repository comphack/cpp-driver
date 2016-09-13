/*
  Copyright (c) 2014-2016 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "request_handler.hpp"

#include "batch_request.hpp"
#include "connection.hpp"
#include "constants.hpp"
#include "error_response.hpp"
#include "execute_request.hpp"
#include "io_worker.hpp"
#include "pool.hpp"
#include "prepare_request.hpp"
#include "response.hpp"
#include "result_response.hpp"
#include "row.hpp"
#include "schema_change_callback.hpp"
#include "session.hpp"

#include <uv.h>

namespace cass {

class PrepareCallback : public RequestCallback {
public:
  PrepareCallback(SpeculativeExecution* speculative_execution)
    : RequestCallback()
    , speculative_execution_(speculative_execution) {}

  bool init(const std::string& prepared_id);

  virtual void on_set(ResponseMessage* response);
  virtual void on_error(CassError code, const std::string& message);
  virtual void on_timeout();

  virtual const Request* request() const { return request_.get(); }
  virtual Request::EncodingCache* encoding_cache() { return &encoding_cache_; }

private:
  ScopedRefPtr<PrepareRequest> request_;
  ScopedRefPtr<SpeculativeExecution> speculative_execution_;
  Request::EncodingCache encoding_cache_;
};

bool PrepareCallback::init(const std::string& prepared_id) {
  request_.reset(new PrepareRequest());
  if (speculative_execution_->request()->opcode() == CQL_OPCODE_EXECUTE) {
    const ExecuteRequest* execute = static_cast<const ExecuteRequest*>(
                                      speculative_execution_->request());
    request_->set_query(execute->prepared()->statement());
    return true;
  } else if (speculative_execution_->request()->opcode() == CQL_OPCODE_BATCH) {
    const BatchRequest* batch = static_cast<const BatchRequest*>(
                                  speculative_execution_->request());
    std::string prepared_statement;
    if (batch->prepared_statement(prepared_id, &prepared_statement)) {
      request_->set_query(prepared_statement);
      return true;
    }
  }
  return false; // Invalid request type
}

void PrepareCallback::on_set(ResponseMessage* response) {
  switch (response->opcode()) {
    case CQL_OPCODE_RESULT: {
      ResultResponse* result =
          static_cast<ResultResponse*>(response->response_body().get());
      if (result->kind() == CASS_RESULT_KIND_PREPARED) {
        speculative_execution_->retry();
      } else {
        speculative_execution_->next_host();
        speculative_execution_->retry();
      }
    } break;
    case CQL_OPCODE_ERROR:
      speculative_execution_->next_host();
      speculative_execution_->retry();
      break;
    default:
      break;
  }
}

void PrepareCallback::on_error(CassError code, const std::string& message) {
  speculative_execution_->next_host();
  speculative_execution_->retry();
}

void PrepareCallback::on_timeout() {
  speculative_execution_->next_host();
  speculative_execution_->retry();
}

void RequestHandler::execute(int64_t timeout) {
  SpeculativeExecution* speculative_execution = new SpeculativeExecution(this, timeout);
  speculative_execution->inc_ref();
  speculative_executions_.push_back(speculative_execution);
}

void RequestHandler::schedule_next_execute(const Host::Ptr& current_host) {
  int64_t timeout = execution_plan_->next_execution(current_host);
  if (timeout >= 0) {
    execute(timeout);
  }
}

void RequestHandler::set_response(const Host::Ptr& host,
                                  const SharedRefPtr<Response>& response) {
  future_->set_response(host->address(), response);
  finish();
}

void RequestHandler::set_error(CassError code,
                               const std::string& message) {
  future_->set_error(code, message);
  finish();
}

void RequestHandler::set_error(const Host::Ptr& host,
                               CassError code, const std::string& message) {
  if (host) {
    future_->set_error_with_address(host->address(), code, message);
  } else {
    future_->set_error(code, message);
  }
  finish();
}

void RequestHandler::set_error_with_error_response(const Host::Ptr& host,
                                                   const SharedRefPtr<Response>& error,
                                                   CassError code, const std::string& message) {
  future_->set_error_with_response(host->address(), error, code, message);
  finish();
}

void RequestHandler::finish() {
  for (SpeculativeExecutionVec::const_iterator i = speculative_executions_.begin(),
       end = speculative_executions_.end(); i != end; ++i) {
    SpeculativeExecution* speculative_execution = *i;
    speculative_execution->cancel();
    speculative_execution->dec_ref();
  }
  dec_ref();
}

SpeculativeExecution::SpeculativeExecution(RequestHandler* request_handler, int64_t timeout)
  : RequestCallback()
  , request_handler_(request_handler)
  , current_host_(request_handler_->current_host())
  , pool_(NULL) {
  if (timeout > 0) {
    timer_.start(request_handler_->io_worker()->loop(), timeout, this, on_execute);
  } else {
    execute(true);
  }
}

void SpeculativeExecution::on_execute(Timer* timer) {
  SpeculativeExecution* speculative_execution = static_cast<SpeculativeExecution*>(timer->data());
  speculative_execution->execute(false);
}

void SpeculativeExecution::on_set(ResponseMessage* response) {
  assert(connection_ != NULL);
  assert(current_host_ && "Tried to set on a non-existent host");
  switch (response->opcode()) {
    case CQL_OPCODE_RESULT:
      on_result_response(response);
      break;
    case CQL_OPCODE_ERROR:
      on_error_response(response);
      break;
    default:
      connection_->defunct();
      set_error(CASS_ERROR_LIB_UNEXPECTED_RESPONSE, "Unexpected response");
      break;
  }
}

void SpeculativeExecution::on_error(CassError code, const std::string& message) {
  if (code == CASS_ERROR_LIB_WRITE_ERROR ||
      code == CASS_ERROR_LIB_UNABLE_TO_SET_KEYSPACE) {
    next_host();
    retry();
    return_connection();
  } else {
    set_error(code, message);
  }
}

void SpeculativeExecution::on_timeout() {
  assert(current_host_ && "Tried to timeout on a non-existent host");
  set_error(CASS_ERROR_LIB_REQUEST_TIMED_OUT, "Request timed out");
}

void SpeculativeExecution::retry() {
  // Reset the request so it can be executed again
  set_state(REQUEST_STATE_NEW);
  pool_ = NULL;
  request_handler_->io_worker()->retry(this);
}

void SpeculativeExecution::cancel() {
  timer_.stop();
}

void SpeculativeExecution::execute(bool use_current_host) {
  request_handler_->schedule_next_execute(current_host_);
  if (!use_current_host) {
    next_host();
  }
  request_handler_->io_worker()->retry(this);
}

void SpeculativeExecution::on_result_response(ResponseMessage* response) {
  ResultResponse* result =
      static_cast<ResultResponse*>(response->response_body().get());
  switch (result->kind()) {
    case CASS_RESULT_KIND_ROWS:
      // Execute statements with no metadata get their metadata from
      // result_metadata() returned when the statement was prepared.
      if (request()->opcode() == CQL_OPCODE_EXECUTE && result->no_metadata()) {
        const ExecuteRequest* execute = static_cast<const ExecuteRequest*>(request());
        if (!execute->skip_metadata()) {
          // Caused by a race condition in C* 2.1.0
          on_error(CASS_ERROR_LIB_UNEXPECTED_RESPONSE, "Expected metadata but no metadata in response (see CASSANDRA-8054)");
          return;
        }
        result->set_metadata(execute->prepared()->result()->result_metadata().get());
      }
      set_response(response->response_body());
      break;

    case CASS_RESULT_KIND_SCHEMA_CHANGE: {
      SharedRefPtr<SchemaChangeCallback> schema_change_handler(
            new SchemaChangeCallback(connection_,
                                    this,
                                    response->response_body()));
      schema_change_handler->execute();
      break;
    }

    case CASS_RESULT_KIND_SET_KEYSPACE:
      request_handler_->io_worker()->broadcast_keyspace_change(result->keyspace().to_string());
      set_response(response->response_body());
      break;

    default:
      set_response(response->response_body());
      break;
  }
}

void SpeculativeExecution::on_error_response(ResponseMessage* response) {
  ErrorResponse* error =
      static_cast<ErrorResponse*>(response->response_body().get());


  switch(error->code()) {
    case CQL_ERROR_UNPREPARED:
      on_error_unprepared(error);
      break;

    case CQL_ERROR_READ_TIMEOUT:
      handle_retry_decision(response,
                            request_handler_->retry_policy()->on_read_timeout(error->consistency(),
                                                                              error->received(),
                                                                              error->required(),
                                                                              error->data_present() > 0,
                                                                              num_retries_));
      break;

    case CQL_ERROR_WRITE_TIMEOUT:
      handle_retry_decision(response,
                            request_handler_->retry_policy()->on_write_timeout(error->consistency(),
                                                                               error->received(),
                                                                               error->required(),
                                                                               error->write_type(),
                                                                               num_retries_));
      break;

    case CQL_ERROR_UNAVAILABLE:
      handle_retry_decision(response,
                            request_handler_->retry_policy()->on_unavailable(error->consistency(),
                                                                             error->required(),
                                                                             error->received(),
                                                                             num_retries_));
      break;

    default:
      set_error(static_cast<CassError>(CASS_ERROR(
                                         CASS_ERROR_SOURCE_SERVER, error->code())),
                error->message().to_string());
      break;
  }
}

void SpeculativeExecution::on_error_unprepared(ErrorResponse* error) {
  ScopedRefPtr<PrepareCallback> prepare_handler(new PrepareCallback(this));
  if (prepare_handler->init(error->prepared_id().to_string())) {
    if (!connection_->write(prepare_handler.get())) {
      // Try to prepare on the same host but on a different connection
      retry();
    }
  } else {
    connection_->defunct();
    set_error(CASS_ERROR_LIB_UNEXPECTED_RESPONSE,
              "Received unprepared error for invalid "
              "request type or invalid prepared id");
  }
}

bool SpeculativeExecution::is_host_up(const Address& address) const {
  return request_handler_->io_worker()->is_host_up(address);
}

void SpeculativeExecution::set_response(const SharedRefPtr<Response>& response) {
  uint64_t elapsed = uv_hrtime() - start_time_ns();
  current_host_->update_latency(elapsed);
  connection_->metrics()->record_request(elapsed);
  request_handler_->set_response(current_host_, response);
  return_connection_and_finish();
}

void SpeculativeExecution::set_error(CassError code, const std::string& message) {
  request_handler_->set_error(current_host_, code, message);
  return_connection_and_finish();
}

void SpeculativeExecution::set_error_with_error_response(const SharedRefPtr<Response>& error,
                                                         CassError code, const std::string& message) {
  request_handler_->set_error_with_error_response(current_host_, error, code, message);
  return_connection_and_finish();
}

void SpeculativeExecution::return_connection() {
  if (pool_ != NULL && connection_ != NULL) {
      pool_->return_connection(connection_);
  }
}

void SpeculativeExecution::return_connection_and_finish() {
  return_connection();
  dec_ref();
}

void SpeculativeExecution::handle_retry_decision(ResponseMessage* response,
                                           const RetryPolicy::RetryDecision& decision) {
  ErrorResponse* error =
      static_cast<ErrorResponse*>(response->response_body().get());

  switch(decision.type()) {
    case RetryPolicy::RetryDecision::RETURN_ERROR:
      set_error_with_error_response(response->response_body(),
                                    static_cast<CassError>(CASS_ERROR(
                                                             CASS_ERROR_SOURCE_SERVER, error->code())),
                                    error->message().to_string());
      break;

    case RetryPolicy::RetryDecision::RETRY:
      set_consistency(decision.retry_consistency());
      if (!decision.retry_current_host()) {
        next_host();
      }
      if (state() == REQUEST_STATE_DONE) {
        retry();
      } else {
        set_state(REQUEST_STATE_RETRY_WRITE_OUTSTANDING);
      }
      break;

    case RetryPolicy::RetryDecision::IGNORE:
      set_response(SharedRefPtr<Response>(new ResultResponse()));
      break;
  }
  num_retries_++;
}

} // namespace cass
