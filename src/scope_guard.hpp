#pragma once
#include <utility>

namespace uvfs
{

//! Runs an action when it goes out of scope, unless it has been dismissed.
//!
//! Used for the writer's temporary file. Cleaning up in catch clauses means
//! the cleanup only happens for the exception types someone remembered to
//! list: commit() caught commit_error and std::runtime_error, so a
//! std::bad_alloc or std::length_error left a full-size temporary behind. A
//! guard cannot be got wrong that way, because it does not need to know what
//! went wrong -- only that the scope was left without success being declared.
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

  //! The work succeeded; do not run the action.
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
