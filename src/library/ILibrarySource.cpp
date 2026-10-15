#include "library/ILibrarySource.h"

namespace mira::library {

Result<void> ILibrarySource::Update(config::Config&, store::GameStore&, api::EventBus&,
                                    const std::string&) {
  return Err("unsupported", Name() + " has no update operation");
}

}  // namespace mira::library
