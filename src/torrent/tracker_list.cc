// libTorrent - BitTorrent library
// Copyright (C) 2005-2011, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#include "config.h"

#include <rak/functional.h>

#include "torrent/utils/log.h"
#include "torrent/utils/log_files.h"
#include "torrent/utils/option_strings.h"
#include "torrent/download_info.h"
#include "net/address_list.h"

#include "globals.h"
#include "exceptions.h"
#include "tracker.h"
#include "tracker_list.h"

#define LT_LOG_TRACKER(log_level, log_fmt, ...)                         \
  lt_log_print_info(LOG_TRACKER_##log_level, info(), "->tracker_list: " log_fmt, __VA_ARGS__);

namespace torrent {

TrackerList::TrackerList() :
  m_info(NULL),
  m_state(DownloadInfo::STOPPED),

  m_key(0),
  m_numwant(-1),
  m_timeLastConnection(0) {
}

bool
TrackerList::has_active() const {
  return std::find_if(begin(), end(), std::mem_fun(&Tracker::is_busy)) != end();
}

// Need a custom predicate because the is_usable function is virtual.
struct tracker_usable_t : public std::unary_function<TrackerList::value_type, bool> {
  bool operator () (const TrackerList::value_type& value) const { return value->is_usable(); }
};

bool
TrackerList::has_usable() const {
  return std::find_if(begin(), end(), tracker_usable_t()) != end();
}

unsigned int
TrackerList::count_active() const {
  return std::count_if(begin(), end(), std::mem_fun(&Tracker::is_busy));
}

unsigned int
TrackerList::count_usable() const {
  return std::count_if(begin(), end(), tracker_usable_t());
}

void
TrackerList::close_all() {
  std::for_each(begin(), end(), std::mem_fun(&Tracker::close));
}

void
TrackerList::clear() {
  std::for_each(begin(), end(), rak::call_delete<Tracker>());
  base_type::clear();
}

void
TrackerList::clear_stats() {
  std::for_each(begin(), end(), std::mem_fun(&Tracker::clear_stats));
}

void
TrackerList::send_state_idx(unsigned idx, int new_event) {
  if (idx >= size())
    throw internal_error("TrackerList::send_state_idx(...) got idx >= size().");
    
  send_state_tracker(begin() + idx, new_event);
}

void
TrackerList::send_state_tracker(iterator itr, int new_event) {
  if (itr == end() || !(*itr)->is_usable())
    return;

  (*itr)->send_state(new_event);

  LT_LOG_TRACKER(DEBUG, "Sending '%s' to group:%u url:'%s'.",
                 option_as_string(OPTION_TRACKER_EVENT, new_event),
                 (*itr)->group(), (*itr)->url().c_str());
}

TrackerList::iterator
TrackerList::insert(unsigned int group, Tracker* tracker) {
  tracker->set_group(group);
  
  iterator itr = base_type::insert(end_group(group), tracker);

  if (m_slot_tracker_enabled)
    m_slot_tracker_enabled(tracker);

  return itr;
}

TrackerList::iterator
TrackerList::find_usable(iterator itr) {
  while (itr != end() && !tracker_usable_t()(*itr))
    ++itr;

  return itr;
}

TrackerList::const_iterator
TrackerList::find_usable(const_iterator itr) const {
  while (itr != end() && !tracker_usable_t()(*itr))
    ++itr;

  return itr;
}

TrackerList::iterator
TrackerList::begin_group(unsigned int group) {
  return std::find_if(begin(), end(), rak::less_equal(group, std::mem_fun(&Tracker::group)));
}

TrackerList::size_type
TrackerList::size_group() const {
  return !empty() ? back()->group() + 1 : 0;
}

void
TrackerList::cycle_group(unsigned int group) {
  iterator itr = begin_group(group);
  iterator prev = itr;

  if (itr == end() || (*itr)->group() != group)
    return;

  while (++itr != end() && (*itr)->group() == group) {
    std::iter_swap(itr, prev);
    prev = itr;
  }
}

TrackerList::iterator
TrackerList::promote(iterator itr) {
  iterator first = begin_group((*itr)->group());

  if (first == end())
    throw internal_error("torrent::TrackerList::promote(...) Could not find beginning of group.");

  std::swap(*first, *itr);
  return first;
}

void
TrackerList::randomize_group_entries() {
  // Random random random.
  iterator itr = begin();
  
  while (itr != end()) {
    iterator tmp = end_group((*itr)->group());
    std::random_shuffle(itr, tmp);

    itr = tmp;
  }
}

void
TrackerList::receive_success(Tracker* tb, AddressList* l) {
  iterator itr = find(tb);

  if (itr == end() || (*itr)->is_busy())
    throw internal_error("TrackerList::receive_success(...) called but the iterator is invalid.");

  // Promote the tracker to the front of the group since it was
  // successfull.
  itr = promote(itr);

  l->sort();
  l->erase(std::unique(l->begin(), l->end()), l->end());

  LT_LOG_TRACKER(INFO, "Received %u peers from tracker url:'%s'.", l->size(), tb->url().c_str());

  tb->set_success_counter(tb->success_counter() + 1);
  tb->set_failed_counter(0);

  set_time_last_connection(cachedTime.seconds());
  m_slot_success(tb, l);
}

void
TrackerList::receive_failed(Tracker* tb, const std::string& msg) {
  iterator itr = find(tb);

  if (itr == end() || tb->is_busy())
    throw internal_error("TrackerList::receive_failed(...) called but the iterator is invalid.");

  LT_LOG_TRACKER(INFO, "Failed to connect to tracker url:'%s' msg:'%s'.", tb->url().c_str(), msg.c_str());

  tb->set_failed_counter(tb->failed_counter() + 1);
  m_slot_failed(tb, msg);
}

}
