#pragma once
#include <utility>

namespace uvfs
{

//! Runs an action on scope exit unless dismissed. Used for the writer's
//! temporary file, so cleanup does not depend on listing exception types.
template <typename F>
class scope_guard
{
public:
  explicit scope_guard(F action) noexcept
      : action_{std::move(action)}
  {
  }
  scope_guard(const scope_guard&) = delete;
  auto operator=(const scope_guard&) -> scope_guard& = delete;
  scope_guard(scope_guard&&) = delete;
  auto operator=(scope_guard&&) -> scope_guard& = delete;

  void dismiss() noexcept { active_ = false; }

  ~scope_guard()
  {
    if (active_)
      action_();
  }

private:
  F action_;
  bool active_{true};
};

template <typename F>
scope_guard(F) -> scope_guard<F>;

} // namespace uvfs
