/*
utrack is a very small an efficient BitTorrent tracker
Copyright (C) 2010-2013  Arvid Norberg

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "announce_thread.hpp"
#include "socket.hpp"
#include "config.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <algorithm> // for generate

#include <signal.h>

#ifndef _WIN32
#include <unistd.h>
#include <netinet/in.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using std::chrono::steady_clock;
using std::chrono::seconds;

extern std::atomic<uint32_t> bytes_out;
extern std::atomic<uint32_t> announces;
extern std::atomic<uint32_t> dropped_announces;
extern std::atomic<uint32_t> scrapes;

std::array<uint8_t, 16> gen_random_key()
{
	std::array<uint8_t, 16> ret;
	std::random_device dev;
	std::generate(ret.begin(), ret.end(), std::ref(dev));
	return ret;
}

#ifdef USE_PCAP
announce_thread::announce_thread(packet_socket& s)
	: m_sock(s)
	, m_quit(false)
	, m_queue_size(0)
	, m_thread( [=]() { thread_fun(); } )
{
	m_queue.reserve(announce_queue_size);
}
#else
announce_thread::announce_thread(int listen_port)
	: m_sock(listen_port)
	, m_quit(false)
	, m_queue_size(0)
	, m_thread( [=]() { thread_fun(); } )
{
	m_queue.reserve(announce_queue_size);
}
#endif

void announce_thread::thread_fun()
{
#ifndef _WIN32
	sigset_t sig;
	sigfillset(&sig);
	int r = pthread_sigmask(SIG_BLOCK, &sig, NULL);
	if (r == -1)
	{
		fprintf(stderr, "pthread_sigmask failed (%d): %s\n", errno, strerror(errno));
	}
#endif

	std::random_device dev;
	std::mt19937 mt_engine(dev());
	std::uniform_int_distribution<int> rand(0, 240);

	// this is the queue the other one is swapped into
	// and then drained without needing to hold the mutex
	std::vector<std::vector<announce_msg>> queue;

	steady_clock::time_point now = steady_clock::now();
	steady_clock::time_point next_prune = now + seconds(10);

	// round-robin for timing out peers
	swarm_map_t::iterator next_to_purge = m_swarms.begin();

	packet_buffer send_buffer(m_sock);

	for (;;)
	{
		std::unique_lock<std::mutex> l(m_mutex);
		while (m_queue.empty()
			&& !m_quit
			&& (now = steady_clock::now()) < next_prune)
			m_cond.wait(l);

		if (m_quit) break;
		m_queue.swap(queue);
		m_queue_size = 0;
		l.unlock();

		now = steady_clock::now();
		// if it's been long enough, just do some relgular
		// maintanence on the swarms
		if (now > next_prune)
		{
			next_prune = now + seconds(10);

			if (next_to_purge == m_swarms.end() && m_swarms.size() > 0)
				next_to_purge = m_swarms.begin();

			if (m_swarms.size() > 0)
			{
				int num_to_purge = (std::min)(int(m_swarms.size()), 20);

				for (int i = 0; i < num_to_purge; ++i)
				{
					swarm& s = next_to_purge->second;
					s.purge_stale(now);

					++next_to_purge;
					if (next_to_purge == m_swarms.end()) next_to_purge = m_swarms.begin();
				}
			}
		}

		for (std::vector<announce_msg> const& v : queue)
		{
			for (announce_msg const& m : v)
			{
				switch (ntohl(m.bits.announce.action))
				{
					case action_announce:
					{
						// find the swarm being announce to
						// or create it if it doesn't exist
						swarm& s = m_swarms[m.bits.announce.hash];

						// prepare the buffer to write the response to
						char* buf;
						int len;
						udp_announce_response resp;

						resp.action = htonl(action_announce);
						resp.transaction_id = m.bits.announce.transaction_id;
						resp.interval = htonl(1680 + rand(mt_engine));

						// do the actual announce with the swarm
						// and get a pointer to the peers back
						s.announce(now, &m.bits.announce, &buf, &len, &resp.downloaders
							, &resp.seeds, mt_engine);

						announces.fetch_add(1, std::memory_order_relaxed);

						// now turn these counters into network byte order
						resp.downloaders = htonl(resp.downloaders);
						resp.seeds = htonl(resp.seeds);

						// set up the iovec array for the response. The header + the
						// body with the peer list
						iovec iov[2] = { { &resp, 20}, { buf, size_t(len) } };

						send_buffer.append(iov, 2, &m.from);
						break;
					}
					case action_scrape:
					{
						udp_scrape_response resp;
						resp.action = htonl(action_scrape);
						resp.transaction_id = m.bits.scrape.transaction_id;

						scrapes.fetch_add(1, std::memory_order_relaxed);

						swarm_map_t::iterator j = m_swarms.find(m.bits.scrape.hash[0]);
						if (j != m_swarms.end())
						{
							j->second.scrape(&resp.data[0].seeds, &resp.data[0].download_count
								, &resp.data[0].downloaders);
							resp.data[0].seeds = htonl(resp.data[0].seeds);
							resp.data[0].download_count = htonl(resp.data[0].download_count);
							resp.data[0].downloaders = htonl(resp.data[0].downloaders);
						}

						iovec iov = { &resp, 8 + 12};
						send_buffer.append(&iov, 1, &m.from);
						break;
					}
				}
			}
		}
		queue.clear();
		m_sock.send(send_buffer);
	}
}

void announce_thread::post_announces(std::vector<announce_msg> m)
{
	if (m.empty()) return;

	std::unique_lock<std::mutex> l(m_mutex);

	// have some upper limit here, to avoid
	// allocating memory indefinitely
	if (m_queue_size >= announce_queue_size)
	{
		dropped_announces.fetch_add(m.size(), std::memory_order_relaxed);
		return;
	}

	m_queue_size += m.size();
	bool first_insert = m_queue.empty();
	m_queue.emplace_back(std::move(m));

	// don't send a signal if we don't need to
	// it's expensive
	if (first_insert)
		m_cond.notify_one();
}

announce_thread::~announce_thread()
{
	std::unique_lock<std::mutex> l(m_mutex);
	m_quit = true;
	l.unlock();
	m_cond.notify_one();
	m_thread.join();
}

