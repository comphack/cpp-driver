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

#ifndef __CASS_SPECULATIVE_EXECUTION_HPP_INCLUDED__
#define __CASS_SPECULATIVE_EXECUTION_HPP_INCLUDED__

#include "host.hpp"
#include "ref_counted.hpp"
#include "scoped_ptr.hpp"

#include <string>
#include <stdint.h>

namespace cass {

class Request;

class SpeculativeExecutionPlan {
public:
  virtual ~SpeculativeExecutionPlan() { }

  virtual int64_t next_execution(const Host::Ptr& current_host) = 0;
};

class SpeculativeExecutionPolicy : public RefCounted<SpeculativeExecutionPolicy> {
public:
  virtual ~SpeculativeExecutionPolicy() { }

  virtual SpeculativeExecutionPlan* new_plan(const std::string& keyspace,
                                             const Request* request) = 0;

  virtual SpeculativeExecutionPolicy* new_instance() = 0;
};

class NoSpeculativeExecutionPlan : public SpeculativeExecutionPlan {
  virtual int64_t next_execution(const Host::Ptr& current_host) { return -1; }
};

class NoSpeculativeExecutionPolicy : public SpeculativeExecutionPolicy {
public:
  NoSpeculativeExecutionPolicy()
    : SpeculativeExecutionPolicy() { }

  virtual SpeculativeExecutionPlan* new_plan(const std::string& keyspace,
                                             const Request* request) {
    return new NoSpeculativeExecutionPlan();
  }

  virtual SpeculativeExecutionPolicy* new_instance()  { return this; }
};

} // namespace cass

#endif
