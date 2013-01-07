/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "Queue.hxx"
#include "song.h"

#include <stdlib.h>

queue::queue(unsigned _max_length)
	:max_length(_max_length), length(0),
	 version(1),
	 items(g_new(struct queue_item, max_length)),
	 order((unsigned *)g_malloc(sizeof(order[0]) * max_length)),
	 id_to_position((int *)g_malloc(sizeof(id_to_position[0]) *
					max_length * QUEUE_HASH_MULT)),
	 repeat(false),
	 single(false),
	 consume(false),
	 random(false)
{
	for (unsigned i = 0; i < max_length * QUEUE_HASH_MULT; ++i)
		id_to_position[i] = -1;
}

queue::~queue()
{
	Clear();

	g_free(items);
	g_free(order);
	g_free(id_to_position);
}

/**
 * Generate a non-existing id number.
 */
static unsigned
queue_generate_id(const struct queue *queue)
{
	static unsigned cur = (unsigned)-1;

	do {
		cur++;

		if (cur >= queue->max_length * QUEUE_HASH_MULT)
			cur = 0;
	} while (queue->id_to_position[cur] != -1);

	return cur;
}

int
queue::GetNextOrder(unsigned _order) const
{
	assert(_order < length);

	if (single && repeat && !consume)
		return _order;
	else if (_order + 1 < length)
		return _order + 1;
	else if (repeat && (_order > 0 || !consume))
		/* restart at first song */
		return 0;
	else
		/* end of queue */
		return -1;
}

void
queue::IncrementVersion()
{
	static unsigned long max = ((uint32_t) 1 << 31) - 1;

	version++;

	if (version >= max) {
		for (unsigned i = 0; i < length; i++)
			items[i].version = 0;

		version = 1;
	}
}

void
queue::ModifyAtOrder(unsigned _order)
{
	assert(_order < length);

	unsigned position = order[_order];
	items[position].version = version;

	IncrementVersion();
}

void
queue::ModifyAll()
{
	for (unsigned i = 0; i < length; i++)
		items[i].version = version;

	IncrementVersion();
}

unsigned
queue::Append(struct song *song, uint8_t priority)
{
	unsigned id = queue_generate_id(this);

	assert(!IsFull());

	auto &item = items[length];
	item.song = song_dup_detached(song);
	item.id = id;
	item.version = version;
	item.priority = priority;

	order[length] = length;
	id_to_position[id] = length;

	++length;

	return id;
}

void
queue::SwapPositions(unsigned position1, unsigned position2)
{
	unsigned id1 = items[position1].id;
	unsigned id2 = items[position2].id;

	std::swap(items[position1], items[position2]);

	items[position1].version = version;
	items[position2].version = version;

	id_to_position[id1] = position2;
	id_to_position[id2] = position1;
}

static void
queue_move_song_to(struct queue *queue, unsigned from, unsigned to)
{
	unsigned from_id = queue->items[from].id;

	queue->items[to] = queue->items[from];
	queue->items[to].version = queue->version;
	queue->id_to_position[from_id] = to;
}

void
queue::MovePostion(unsigned from, unsigned to)
{
	struct queue_item item = items[from];

	/* move songs to one less in from->to */

	for (unsigned i = from; i < to; i++)
		queue_move_song_to(this, i + 1, i);

	/* move songs to one more in to->from */

	for (unsigned i = from; i > to; i--)
		queue_move_song_to(this, i - 1, i);

	/* put song at _to_ */

	id_to_position[item.id] = to;
	items[to] = item;
	items[to].version = version;

	/* now deal with order */

	if (random) {
		for (unsigned i = 0; i < length; i++) {
			if (order[i] > from && order[i] <= to)
				order[i]--;
			else if (order[i] < from &&
				 order[i] >= to)
				order[i]++;
			else if (from == order[i])
				order[i] = to;
		}
	}
}

void
queue::MoveRange(unsigned start, unsigned end, unsigned to)
{
	struct queue_item tmp[end - start];
	// Copy the original block [start,end-1]
	for (unsigned i = start; i < end; i++)
		tmp[i - start] = items[i];

	// If to > start, we need to move to-start items to start, starting from end
	for (unsigned i = end; i < end + to - start; i++)
		queue_move_song_to(this, i, start + i - end);

	// If to < start, we need to move start-to items to newend (= end + to - start), starting from to
	// This is the same as moving items from start-1 to to (decreasing), with start-1 going to end-1
	// We have to iterate in this order to avoid writing over something we haven't yet moved
	for (unsigned i = start - 1; i >= to && i != G_MAXUINT; i--)
		queue_move_song_to(this, i, i + end - start);

	// Copy the original block back in, starting at to.
	for (unsigned i = start; i< end; i++)
	{
		id_to_position[tmp[i-start].id] = to + i - start;
		items[to + i - start] = tmp[i-start];
		items[to + i - start].version = version;
	}

	if (random) {
		// Update the positions in the queue.
		// Note that the ranges for these cases are the same as the ranges of
		// the loops above.
		for (unsigned i = 0; i < length; i++) {
			if (order[i] >= end && order[i] < to + end - start)
				order[i] -= end - start;
			else if (order[i] < start &&
				 order[i] >= to)
				order[i] += end - start;
			else if (start <= order[i] && order[i] < end)
				order[i] += to - start;
		}
	}
}

/**
 * Moves a song to a new position in the "order" list.
 */
static void
queue_move_order(struct queue *queue, unsigned from_order, unsigned to_order)
{
	assert(queue != NULL);
	assert(from_order < queue->length);
	assert(to_order <= queue->length);

	const unsigned from_position = queue->OrderToPosition(from_order);

	if (from_order < to_order) {
		for (unsigned i = from_order; i < to_order; ++i)
			queue->order[i] = queue->order[i + 1];
	} else {
		for (unsigned i = from_order; i > to_order; --i)
			queue->order[i] = queue->order[i - 1];
	}

	queue->order[to_order] = from_position;
}

void
queue::DeletePosition(unsigned position)
{
	assert(position < length);

	struct song *song = Get(position);
	assert(!song_in_database(song) || song_is_detached(song));
	song_free(song);

	const unsigned id = PositionToId(position);
	const unsigned _order = PositionToOrder(position);

	--length;

	/* release the song id */

	id_to_position[id] = -1;

	/* delete song from songs array */

	for (unsigned i = position; i < length; i++)
		queue_move_song_to(this, i + 1, i);

	/* delete the entry from the order array */

	for (unsigned i = _order; i < length; i++)
		order[i] = order[i + 1];

	/* readjust values in the order array */

	for (unsigned i = 0; i < length; i++)
		if (order[i] > position)
			--order[i];
}

void
queue::Clear()
{
	for (unsigned i = 0; i < length; i++) {
		struct queue_item *item = &items[i];

		assert(!song_in_database(item->song) ||
		       song_is_detached(item->song));
		song_free(item->song);

		id_to_position[item->id] = -1;
	}

	length = 0;
}

static const struct queue_item *
queue_get_order_item_const(const struct queue *queue, unsigned order)
{
	assert(queue != NULL);
	assert(order < queue->length);

	return &queue->items[queue->order[order]];
}

static uint8_t
queue_get_order_priority(const struct queue *queue, unsigned order)
{
	return queue_get_order_item_const(queue, order)->priority;
}

static gint
queue_item_compare_order_priority(gconstpointer av, gconstpointer bv,
				  gpointer user_data)
{
	const struct queue *queue = (const struct queue *)user_data;
	const unsigned *const ap = (const unsigned *)av;
	const unsigned *const bp = (const unsigned *)bv;
	assert(ap >= queue->order && ap < queue->order + queue->length);
	assert(bp >= queue->order && bp < queue->order + queue->length);
	uint8_t a = queue->items[*ap].priority;
	uint8_t b = queue->items[*bp].priority;

	if (G_LIKELY(a == b))
		return 0;
	else if (a > b)
		return -1;
	else
		return 1;
}

static void
queue_sort_order_by_priority(struct queue *queue, unsigned start, unsigned end)
{
	assert(queue != NULL);
	assert(queue->random);
	assert(start <= end);
	assert(end <= queue->length);

	g_qsort_with_data(&queue->order[start], end - start,
			  sizeof(queue->order[0]),
			  queue_item_compare_order_priority,
			  queue);
}

void
queue::ShuffleOrderRange(unsigned start, unsigned end)
{
	assert(random);
	assert(start <= end);
	assert(end <= length);

	rand.AutoCreate();
	std::shuffle(order + start, order + end, rand);
}

/**
 * Sort the "order" of items by priority, and then shuffle each
 * priority group.
 */
void
queue::ShuffleOrderRangeWithPriority(unsigned start, unsigned end)
{
	assert(random);
	assert(start <= end);
	assert(end <= length);

	if (start == end)
		return;

	/* first group the range by priority */
	queue_sort_order_by_priority(this, start, end);

	/* now shuffle each priority group */
	unsigned group_start = start;
	uint8_t group_priority = queue_get_order_priority(this, start);

	for (unsigned i = start + 1; i < end; ++i) {
		uint8_t priority = queue_get_order_priority(this, i);
		assert(priority <= group_priority);

		if (priority != group_priority) {
			/* start of a new group - shuffle the one that
			   has just ended */
			ShuffleOrderRange(group_start, i);
			group_start = i;
			group_priority = priority;
		}
	}

	/* shuffle the last group */
	ShuffleOrderRange(group_start, end);
}

void
queue::ShuffleOrder()
{
	ShuffleOrderRangeWithPriority(0, length);
}

void
queue::ShuffleOrderFirst(unsigned start, unsigned end)
{
	rand.AutoCreate();

	std::uniform_int_distribution<unsigned> distribution(start, end - 1);
	SwapOrders(start, distribution(rand));
}

void
queue::ShuffleOrderLast(unsigned start, unsigned end)
{
	rand.AutoCreate();

	std::uniform_int_distribution<unsigned> distribution(start, end - 1);
	SwapOrders(end - 1, distribution(rand));
}

void
queue::ShuffleRange(unsigned start, unsigned end)
{
	assert(start <= end);
	assert(end <= length);

	rand.AutoCreate();

	for (unsigned i = start; i < end; i++) {
		std::uniform_int_distribution<unsigned> distribution(start,
								     end - 1);
		unsigned ri = distribution(rand);
		SwapPositions(i, ri);
	}
}

/**
 * Find the first item that has this specified priority or higher.
 */
G_GNUC_PURE
static unsigned
queue_find_priority_order(const struct queue *queue, unsigned start_order,
			  uint8_t priority, unsigned exclude_order)
{
	assert(queue != NULL);
	assert(queue->random);
	assert(start_order <= queue->length);

	for (unsigned order = start_order; order < queue->length; ++order) {
		const unsigned position = queue->OrderToPosition(order);
		const struct queue_item *item = &queue->items[position];
		if (item->priority <= priority && order != exclude_order)
			return order;
	}

	return queue->length;
}

G_GNUC_PURE
static unsigned
queue_count_same_priority(const struct queue *queue, unsigned start_order,
			  uint8_t priority)
{
	assert(queue != NULL);
	assert(queue->random);
	assert(start_order <= queue->length);

	for (unsigned order = start_order; order < queue->length; ++order) {
		const unsigned position = queue->OrderToPosition(order);
		const struct queue_item *item = &queue->items[position];
		if (item->priority != priority)
			return order - start_order;
	}

	return queue->length - start_order;
}

bool
queue::SetPriority(unsigned position, uint8_t priority, int after_order)
{
	assert(position < length);

	struct queue_item *item = &items[position];
	uint8_t old_priority = item->priority;
	if (old_priority == priority)
		return false;

	item->version = version;
	item->priority = priority;

	if (!random)
		/* don't reorder if not in random mode */
		return true;

	unsigned _order = PositionToOrder(position);
	if (after_order >= 0) {
		if (_order == (unsigned)after_order)
			/* don't reorder the current song */
			return true;

		if (_order < (unsigned)after_order) {
			/* the specified song has been played already
			   - enqueue it only if its priority has just
			   become bigger than the current one's */

			const unsigned after_position =
				OrderToPosition(after_order);
			const struct queue_item *after_item =
				&items[after_position];
			if (old_priority > after_item->priority ||
			    priority <= after_item->priority)
				/* priority hasn't become bigger */
				return true;
		}
	}

	/* move the item to the beginning of the priority group (or
	   create a new priority group) */

	const unsigned before_order =
		queue_find_priority_order(this, after_order + 1, priority,
					  _order);
	const unsigned new_order = before_order > _order
		? before_order - 1
		: before_order;
	queue_move_order(this, _order, new_order);

	/* shuffle the song within that priority group */

	const unsigned priority_count =
		queue_count_same_priority(this, new_order, priority);
	assert(priority_count >= 1);
	ShuffleOrderFirst(new_order, new_order + priority_count);

	return true;
}

bool
queue::SetPriorityRange(unsigned start_position, unsigned end_position,
			uint8_t priority, int after_order)
{
	assert(start_position <= end_position);
	assert(end_position <= length);

	bool modified = false;
	int after_position = after_order >= 0
		? (int)OrderToPosition(after_order)
		: -1;
	for (unsigned i = start_position; i < end_position; ++i) {
		after_order = after_position >= 0
			? (int)PositionToOrder(after_position)
			: -1;

		modified |= SetPriority(i, priority, after_order);
	}

	return modified;
}