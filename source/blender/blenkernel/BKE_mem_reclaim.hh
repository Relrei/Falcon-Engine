/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Falcon: hand the memory that the allocator is holding on to back to the OS.
 *
 * Blender links `libtbbmalloc_proxy`, which replaces `malloc`/`free` process wide.
 * The TBB scalable allocator keeps freed blocks in per-thread bins instead of
 * returning them, so after a render Blender's own book-keeping
 * (`wm.memory_statistics`) is back to the startup value while the RSS is still
 * 0.8-1.5 GB higher. `malloc_trim()` does not help because glibc's arena is not
 * the one being used.
 *
 * `scalable_allocation_command(TBBMALLOC_CLEAN_ALL_BUFFERS, ...)` is the TBB side
 * of `malloc_trim()`. It is looked up with `dlsym()` so this is a no-op in a build
 * without the TBB malloc proxy, and so that nothing here needs to link TBB.
 *
 * This is *not* a leak fix: everything released here was already freed. It only
 * changes when the pages go back to the OS, so it must only be called at coarse
 * boundaries (end of a render, end of a file read) and never inside a loop --
 * the next allocations have to fault the pages back in.
 *
 * `FALCON_MEM_RECLAIM=0` turns it off (the behaviour before this was added).
 */

namespace blender::bke {

/**
 * Return the allocator's free buffers to the OS. Cheap when there is nothing to
 * return. Safe to call when TBB is not in use (does nothing).
 */
void mem_reclaim_to_os(const char *where);

/**
 * Reclaims on destruction, but only when the outermost scope on this thread ends.
 *
 * Renders nest: Falcon LT runs one extra `RE_RenderFrame()` per light for the
 * caustics, so a single F12 on `classroom` goes through the render pipeline five
 * times. Reclaiming between those passes is worse than not reclaiming at all --
 * the buffers thrown away are needed again immediately, and it measured as
 * +472 MB RSS. `G.is_rendering` cannot be used to detect this: the inner render
 * clears it on the way out, so only the first nested call looks nested.
 */
class MemReclaimScope {
  const char *where_;

 public:
  explicit MemReclaimScope(const char *where);
  ~MemReclaimScope();

  MemReclaimScope(const MemReclaimScope &) = delete;
  MemReclaimScope &operator=(const MemReclaimScope &) = delete;
};

}  // namespace blender::bke
