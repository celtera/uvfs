#pragma once
#include <type_traits>
#include <utility>

namespace uvfs
{

//! A non-owning reference to a callable. Unlike std::function it never
//! allocates and never copies the callable, which matters for a per-entry
//! callback: the old for_each_file paid an indirect call and a possible heap
//! allocation for something that is always a stack lambda at the call site.
template <typename Signature>
struct function_ref;

template <typename R, typename... Args>
struct function_ref<R(Args...)>
{
  template <
      typename F,
      typename = std::enable_if_t<
          !std::is_same_v<std::decay_t<F>, function_ref>
          && std::is_invocable_r_v<R, F&, Args...>>>
  function_ref(F&& f) noexcept
      : object{const_cast<void*>(static_cast<const void*>(std::addressof(f)))}
      , invoke{[](void* obj, Args... args) -> R {
        return (*static_cast<std::remove_reference_t<F>*>(obj))(
            std::forward<Args>(args)...);
      }}
  {
  }

  auto operator()(Args... args) const -> R
  {
    return invoke(object, std::forward<Args>(args)...);
  }

private:
  void* object{};
  R (*invoke)(void*, Args...){};
};

} // namespace uvfs
