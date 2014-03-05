/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
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
#include "DoubanFMPlaylistPlugin.hxx"
#include "../PlaylistPlugin.hxx"
#include "../SongEnumerator.hxx"
#include "config/ConfigData.hxx"
#include "input/InputStream.hxx"
#include "tag/TagBuilder.hxx"
#include "tag/Tag.hxx"
#include "util/StringUtil.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include "DetachedSong.hxx"

#include <glib.h>
#include <yajl/yajl_parse.h>

#include <string>

#include <string.h>
#include <forward_list>

static constexpr Domain doubanfm_domain("doubanfm");

/* YAJL parser for track data JSON */

enum key {
	Duration,
	Title,
	Stream_URL,
	ARTIST,
	SID,
	Other,
};

const char* doubanfm_key_str[] = {
	"length",
	"title",
	"url",
	"artist",
	"sid",
	nullptr,
};

struct parse_data {
	int key;
	char* stream_url;
	long duration;
	char* title;
	char* artist;
	int got_url; /* nesting level of last stream_url */

	char* history;
	std::forward_list<DetachedSong> songs;
};

static int
handle_integer(void *ctx,
	       long long
	       intval)
{
	struct parse_data *data = (struct parse_data *) ctx;

	switch (data->key) {
	case Duration:
		data->duration = intval;
		break;
	default:
		break;
	}

	return 1;
}

static int
handle_string(void *ctx, const unsigned char* stringval,
	      size_t
	      stringlen)
{
	struct parse_data *data = (struct parse_data *) ctx;
	const char *s = (const char *) stringval;

	switch (data->key) {
	case Title:
		g_free(data->title);
		data->title = g_strndup(s, stringlen);
		break;
	case Stream_URL:
		g_free(data->stream_url);
		data->stream_url = g_strndup(s, stringlen);
		data->got_url = 1;
		break;
	case ARTIST:
		g_free(data->artist);
		data->artist = g_strndup(s, stringlen);
		break;
	case SID:
		if (data->history == nullptr){
			char* newsong = g_strndup(s, stringlen);
			data->history = g_strconcat(newsong,":p",nullptr);
			g_free(newsong);
		}else{
			char* origin = data->history;
			char* newsong = g_strndup(s, stringlen);
			data->history = g_strconcat(origin,"|",newsong,":p",nullptr);
			g_free(origin);
			g_free(newsong);
		}

		break;
	default:
		break;
	}

	return 1;
}

static int
handle_mapkey(void *ctx, const unsigned char* stringval,
	      size_t
	      stringlen)
{
	struct parse_data *data = (struct parse_data *) ctx;

	int i;
	data->key = Other;

	for (i = 0; i < Other; ++i) {
		if (memcmp((const char *)stringval, doubanfm_key_str[i], stringlen) == 0) {
			data->key = i;
			break;
		}
	}

	return 1;
}

static int
handle_start_map(void *ctx)
{
	struct parse_data *data = (struct parse_data *) ctx;

	if (data->got_url > 0)
		data->got_url++;

	return 1;
}

static int
handle_end_map(void *ctx)
{
	struct parse_data *data = (struct parse_data *) ctx;

	if (data->got_url > 1) {
		data->got_url--;
		return 1;
	}

	if (data->got_url == 0)
		return 1;

	/* got_url == 1, track finished, make it into a song */
	data->got_url = 0;

	char *u = g_strconcat(data->stream_url, nullptr);

	TagBuilder tag;
	tag.SetTime(data->duration);
	if (data->title != nullptr)
		tag.AddItem(TAG_NAME, data->title);
	if (data->artist != nullptr)
		tag.AddItem(TAG_ARTIST, data->artist);

	data->songs.emplace_front(u, tag.Commit());
	g_free(u);

	FormatWarning(doubanfm_domain, "Found music %s", data->title);

	return 1;
}

static yajl_callbacks parse_callbacks = {
	nullptr,
	nullptr,
	handle_integer,
	nullptr,
	nullptr,
	handle_string,
	handle_start_map,
	handle_mapkey,
	handle_end_map,
	nullptr,
	nullptr,
};

static struct {
	char *user;
	char *password;
	int onceTotal;
} doubanfm_config;

class DoubanFMPlaylist final : public SongEnumerator {
	struct parse_data data;
	char* url;
	Mutex &mutex;
	Cond &cond;
	int count = 0;

public:
	DoubanFMPlaylist(char* _url, Mutex &_mutex, Cond &_cond)
		:url(std::move(_url)), mutex(_mutex), cond(_cond) {
		data.history = nullptr;
	}

	virtual ~DoubanFMPlaylist() {
		g_free(url);
		g_free(data.history);
	}

	int getNewSongs(){
		Error error;
		char* realurl = g_strconcat(url, data.history, nullptr);
		InputStream *input_stream = InputStream::OpenReady(realurl, mutex, cond,
				error);
		if (input_stream == nullptr) {
			if (error.IsDefined())
				LogError(error);
			return -1;
		}

		mutex.lock();

		yajl_status stat;
		bool done = false;
		data.got_url = 0;
		data.title = nullptr;
		data.stream_url = nullptr;
		data.artist = nullptr;

		yajl_handle hand = yajl_alloc(&parse_callbacks, nullptr, &data);

		while (!done) {
			char buffer[4096];
			unsigned char *ubuffer = (unsigned char *)buffer;
			const size_t nbytes =
				input_stream->Read(buffer, sizeof(buffer), error);
			if (nbytes == 0) {
				if (error.IsDefined())
					LogError(error);

				if (input_stream->IsEOF()) {
					done = true;
				} else {
					mutex.unlock();
					input_stream->Close();
					return -1;
				}
			}

			if (done) {
				stat = yajl_complete_parse(hand);
			} else
				stat = yajl_parse(hand, ubuffer, nbytes);

			if (stat != yajl_status_ok)
			{
				unsigned char *str = yajl_get_error(hand, 1, ubuffer, nbytes);
				LogError(doubanfm_domain, (const char *)str);
				yajl_free_error(hand, str);
				break;
			}
		}

		data.songs.reverse();
		mutex.unlock();

		input_stream->Close();

		g_free(data.title);
		g_free(data.stream_url);
		g_free(data.artist);

		yajl_free(hand);

		g_free(realurl);

		return 0;
	}

	virtual DetachedSong *NextSong() override {
		if (data.songs.empty()){
			if (count < doubanfm_config.onceTotal){
				if (getNewSongs() == -1){
					return nullptr;
				}
			}else
				return nullptr;
		}

		auto result = new DetachedSong(std::move(data.songs.front()));
		data.songs.pop_front();
		count++;
		return result;
	}
};

static bool
doubanfm_init(const config_param &param)
{
	const char *user = param.GetBlockValue("user");
	const char *passwd = param.GetBlockValue("password");
	const char onceTotal = param.GetBlockValue("onceAdd", 20);

	/* currently don't support personal channel
	if (user == NULL || passwd == NULL) {
		LogWarning(doubanfm_domain,
			 "disabling the doubanfm playlist plugin "
			 "because account is not configured");
		return false;
	}
	*/

	doubanfm_config.user = g_uri_escape_string(user, NULL, false);
	doubanfm_config.password = g_uri_escape_string(passwd, NULL, false);
	doubanfm_config.onceTotal = onceTotal;

	return true;
}

static void
doubanfm_finish(void){
	g_free(doubanfm_config.user);
	g_free(doubanfm_config.password);
}


/**
 * Parse a doubanfm:// URL and create a playlist.
 * @param uri A doubanfm URL. Accepted forms:
 *	doubanfm://track/<track-id>
 *	doubanfm://playlist/<playlist-id>
 *	doubanfm://url/<url or path of doubanfm page>
 */
static SongEnumerator *
doubanfm_open_uri(const char *uri, Mutex &mutex, Cond &cond)
{
	assert(memcmp(uri, "doubanfm://", 11) == 0);
	uri += 11;

	char *u = nullptr;
	if (memcmp(uri, "channel/", 8) == 0) {
		const char *rest = uri + 8;
		u = g_strconcat("http://www.douban.com/j/app/radio/people?app_name=radio_desktop_win&version=100&user_id=&expire=&token=&sid=&channel=",
				rest, "&type=n&h=", nullptr);
	} else if (memcmp(uri, "url/", 4) == 0) {
		const char *rest = uri + 4;
		u = g_strdup(rest);
	}

	if (u == nullptr) {
		LogWarning(doubanfm_domain, "unknown doubanfm URI");
		return nullptr;
	}

	return new DoubanFMPlaylist(u, mutex, cond);
}

static const char *const doubanfm_schemes[] = {
	"doubanfm",
	nullptr
};

const struct playlist_plugin doubanfm_playlist_plugin = {
	"doubanfm",

	doubanfm_init,
	doubanfm_finish,
	doubanfm_open_uri,
	nullptr,

	doubanfm_schemes,
	nullptr,
	nullptr,
};


