/*
  Copyright 2014 DataStax

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

#ifndef __CASS_RESULT_ITERATOR_HPP_INCLUDED__
#define __CASS_RESULT_ITERATOR_HPP_INCLUDED__

#include "iterable.hpp"
#include "body_result.hpp"

struct MessageBodyResult;

// struct ResultIterator : Iterable {
//   MessageBodyResult* result;
//   int32_t     row_position;
//   char*       position;

//   ResultIterator(
//       MessageBodyResult* result) :
//       Iterable(CASS_ITERABLE_TYPE_RESULT),
//       result(result),
//       row_position(0),
//       position(result->rows)
//   {}

//   inline bool
//   next() {
//     return false;
//   }
// };

#endif
