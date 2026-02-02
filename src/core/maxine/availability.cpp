#include "availability.h"

namespace studiocast::maxine {

bool BackendBuilt() {
#ifdef STUDIOCAST_WITH_MAXINE
  return true;
#else
  return false;
#endif
}

bool RuntimeAvailable(std::string* reason) {
#ifndef STUDIOCAST_WITH_MAXINE
  if (reason) {
    *reason = "Maxine backend not enabled in this build (compile with STUDIOCAST_WITH_MAXINE).";
  }
  return false;
#else
  // TODO: implement dlopen-based runtime loading and feature checks.
  if (reason) {
    *reason = "Maxine backend build flag enabled, but runtime loading is not implemented yet.";
  }
  return false;
#endif
}

}  // namespace studiocast::maxine
