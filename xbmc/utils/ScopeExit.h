/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <type_traits>
#include <utility>

namespace KODI
{
namespace UTILS
{

/*! \class CScopeExit
    \brief Invoke a callable when leaving the current scope.
 */
template<typename ExitFunction>
class CScopeExit
{
public:
  static_assert(std::is_invocable_v<ExitFunction&>, "ExitFunction must be invocable");

  explicit CScopeExit(ExitFunction&& exitFunction) noexcept(
      std::is_nothrow_move_constructible_v<ExitFunction>)
    : m_exitFunction(std::forward<ExitFunction>(exitFunction))
  {
  }

  ~CScopeExit() noexcept
  {
    if (m_active) m_exitFunction();
  }

  CScopeExit(const CScopeExit&) = delete;
  CScopeExit& operator=(const CScopeExit&) = delete;

  CScopeExit(CScopeExit&& other) noexcept(std::is_nothrow_move_constructible_v<ExitFunction>)
    : m_exitFunction(std::move(other.m_exitFunction)), m_active(other.m_active)
  {
    other.release();
  }

  CScopeExit& operator=(CScopeExit&&) = delete;

  void release() noexcept
  {
    m_active = false;
  }

private:
  ExitFunction m_exitFunction;
  bool m_active{true};
};

template<typename ExitFunction>
[[nodiscard]]
auto MakeScopeExit(ExitFunction&& exitFunction)
{
  return CScopeExit<std::decay_t<ExitFunction>>(std::forward<ExitFunction>(exitFunction));
}

} // namespace UTILS
} // namespace KODI