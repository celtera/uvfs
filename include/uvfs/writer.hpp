#pragma once
#include "config.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <string_view>

namespace uvfs
{

// What commit() should do when an input file cannot be read.
enum class on_unreadable
{
  fail, //!< abandon the archive and throw (default)
  skip  //!< leave the file out of the archive and carry on
};

// What to do when the same archive path is added more than once.
enum class on_duplicate
{
  fail,   //!< abandon the archive and throw (default)
  replace //!< keep the last one added
};

//! One input that could not be archived.
struct UVFS_EXPORT skipped_file
{
  std::string path_in_archive;
  std::string path_in_system;
  std::string reason;
};

//! Thrown by commit() when inputs could not be read and the policy is `fail`.
//! The archive is not created; no partial file is left behind.
struct UVFS_EXPORT commit_error : std::runtime_error
{
  explicit commit_error(const std::string& what, std::vector<skipped_file> f)
      : std::runtime_error{what}
      , files{std::move(f)}
  {
  }
  std::vector<skipped_file> files;
};

struct UVFS_EXPORT writer
{
public:
  explicit writer();
  writer(const writer&) = delete;
  writer(writer&&) noexcept = delete;
  auto operator=(const writer&) -> writer& = delete;
  auto operator=(writer&&) noexcept -> writer& = delete;
  ~writer();

  //! Registers a file. The file is not read until commit().
  //! Throws std::invalid_argument if `path_in_archive` is not a valid archive
  //! path; see uvfs/path.hpp for the rule.
  void add_file(std::string_view path_in_archive, std::string_view path_in_system);

  //! Controls what happens when an input cannot be read. Default: fail.
  void set_unreadable_policy(on_unreadable policy) noexcept;

  //! Controls what happens when an archive path is added twice. Default: fail.
  void set_duplicate_policy(on_duplicate policy) noexcept;

  //! Builds the archive. The output appears atomically: it is written to a
  //! temporary file in the same directory and renamed into place, so an
  //! interrupted or failed commit never leaves a partial archive behind.
  void commit(std::string_view path);

  //! Inputs left out of the last commit(), when the policy is `skip`.
  [[nodiscard]] auto skipped() const noexcept -> const std::vector<skipped_file>&;

private:
  struct impl;
  std::unique_ptr<impl> impl;
};
}
