/*
Copyright (C) 2010-201$  Arvid Norberg

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

#include "receive_thread.hpp"
#include "key_rotate.hpp"
#include "messages.hpp"
#include "endian.hpp"
#include "announce_thread.hpp"
#include "config.hpp"

#include <signal.h>

extern std::atomic<uint32_t> connects;
extern std::atomic<uint32_t> errors;
extern std::atomic<uint32_t> bytes_in;

extern key_rotate keys;

extern "C" int  siphash( unsigned char *out, const unsigned char *in
	, unsigned long long inlen, const unsigned char *k);

std::uint64_t gen_secret_digest(sockaddr_in const* from
	, std::array<std::uint8_t, 16> const& key)
{
	std::array<std::uint8_t, sizeof(from->sin_addr) + sizeof(from->sin_port)> ep;
	memcpy(ep.data(), (char*)&from->sin_addr, sizeof(from->sin_addr));
	memcpy(ep.data() + sizeof(from->sin_addr), (char*)&from->sin_port
		, sizeof(from->sin_port));

	std::uint64_t ret;
	siphash((std::uint8_t*)&ret, ep.data(), ep.size(), key.data());
	return ret;
}

uint64_t generate_connection_id(sockaddr_in const* from)
{
	return gen_secret_digest(from, keys.cur_key());
}

bool verify_connection_id(uint64_t conn_id, sockaddr_in const* from)
{
	return conn_id == gen_secret_digest(from, keys.cur_key())
		|| conn_id == gen_secret_digest(from, keys.prev_key());
}

#ifdef USE_PCAP
receive_thread::receive_thread(packet_socket& s
	, std::vector<announce_thread*> const& at)
	: m_sock(s)
	, m_announce_threads(at)
	, m_thread( [=]() { thread_fun(); } ) {}

#else

receive_thread::receive_thread(std::vector<announce_thread*> const& at)
	: m_sock(true)
	, m_send_sock()
	, m_announce_threads(at)
	, m_thread( [=]() { thread_fun(); } ) {}

#endif

receive_thread::~receive_thread()
{
	m_sock.close();
#ifndef USE_PCAP
	m_send_sock.close();
#endif
	m_thread.join();
}

void receive_thread::close()
{
	m_sock.close();
}

void receive_thread::thread_fun()
{
	sigset_t sig;
	sigfillset(&sig);
	int r = pthread_sigmask(SIG_BLOCK, &sig, NULL);
	if (r == -1)
	{
		fprintf(stderr, "pthread_sigmask failed (%d): %s\n", errno, strerror(errno));
	}

#ifdef USE_PCAP
	packet_buffer send_buffer(m_sock);
#else
	packet_buffer send_buffer(m_send_sock);
#endif

	incoming_packet_t pkts[1024];

	for (;;)
	{
		int recvd = m_sock.receive(pkts, sizeof(pkts)/sizeof(pkts[0]));
		if (recvd <= 0) break;
		for (int i = 0; i < recvd; ++i)
			incoming_packet(pkts[i].buffer, pkts[i].buflen
				, (sockaddr_in*)&pkts[i].from, pkts[0].fromlen
				, send_buffer);

#ifdef USE_PCAP
		m_sock.send(send_buffer);
#else
		m_send_sock.send(send_buffer);
#endif
	}
}

// this thread receives incoming announces, parses them and posts
// the announce to the correct announce thread, that then takes over
// and is responsible for responding
void receive_thread::incoming_packet(char const* buf, int size
	, sockaddr_in const* from, socklen_t fromlen
	, packet_buffer& send_buffer)
{
	bytes_in += size;

//	printf("received message from: %x port: %d size: %d\n"
//		, from->sin_addr.s_addr, ntohs(from->sin_port), size);

	if (size < 16)
	{
		printf("packet too short (%d)\n", size);
		// log incorrect packet
		return;
	}

	udp_announce_message* hdr = (udp_announce_message*)buf;

	switch (ntohl(hdr->action))
	{
		case action_connect:
		{
			if (be64toh(hdr->connection_id) != 0x41727101980LL)
			{
				++errors;
				printf("invalid connection ID for connect message\n");
				// log error
				return;
			}
			udp_connect_response resp;
			resp.action = htonl(action_connect);
			resp.connection_id = generate_connection_id(from);
			resp.transaction_id = hdr->transaction_id;
			++connects;
			iovec iov = { &resp, 16};
			if (send_buffer.append(&iov, 1, (sockaddr*)from, fromlen))
				return;
			break;
		}
		case action_announce:
		{
			if (!verify_connection_id(hdr->connection_id, from))
			{
				printf("invalid connection ID\n");
				++errors;
				// log error
				return;
			}
			// technically the announce message should
			// be 100 bytes, but uTorrent doesn't seem to send
			// the extension field at the end
			if (size < 98)
			{
				printf("announce packet too short. Expected 100, got %d\n", size);
				++errors;
				// log incorrect packet
				return;
			}

			if (!allow_alternate_ip || hdr->ip == 0)
				hdr->ip = from->sin_addr.s_addr;

			// post the announce to the thread that's responsible
			// for this info-hash
			announce_msg m;
			m.bits.announce = *hdr;
			m.from = *from;
			m.fromlen = fromlen;

			// use siphash here to prevent hash collision attacks causing one
			// thread to overload
			int thread_selector = siphash_fun()(hdr->hash) % m_announce_threads.size();
			m_announce_threads[thread_selector]->post_announce(m);

			break;
		}
		case action_scrape:
		{
			if (!verify_connection_id(hdr->connection_id, from))
			{
				printf("invalid connection ID for connect message\n");
				++errors;
				// log error
				return;
			}
			if (size < 16 + 20)
			{
				printf("scrape packet too short. Expected 36, got %d\n", size);
				++errors;
				// log error
				return;
			}

			udp_scrape_message* req = (udp_scrape_message*)buf;

			// for now, just support scrapes for a single hash at a time
			// to avoid having to bounce the request around all the threads
			// befor accruing all the stats

			// post the announce to the thread that's responsible
			// for this info-hash
			announce_msg m;
			m.bits.scrape = *req;
			m.from = *from;
			m.fromlen = fromlen;
			int thread_selector = req->hash[0].val[0] % m_announce_threads.size();
			m_announce_threads[thread_selector]->post_announce(m);

			break;
		}
		default:
			printf("unknown action %d\n", ntohl(hdr->action));
			++errors;
			break;
	}
}
