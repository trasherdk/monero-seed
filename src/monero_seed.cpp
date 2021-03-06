/*
	Copyright (c) 2020 tevador <tevador@gmail.com>
	All rights reserved.
*/

#include "monero_seed.hpp"
#include "secure_random.hpp"
#include "wordlist.hpp"
#include "gf_poly.hpp"
#include "reed_solomon_code.hpp"
#include "argon2/argon2.h"
#include "argon2/blake2/blake2-impl.h"
#include <chrono>
#include <cassert>
#include <stdexcept>
#include <cstdint>
#include <climits>
#include <iomanip>
#include <sstream>
#include <algorithm>

const std::string monero_seed::erasure = "xxxx";

class monero_seed_exception : public std::exception {
public:
	monero_seed_exception(const std::string& msg)
		: msg_(msg)
	{ }
	~monero_seed_exception() throw() {}

	const char* what() const throw() override {
		return msg_.c_str();
	}
private:
	std::string msg_;
};

#define THROW_EXCEPTION(message) do { \
		std::ostringstream oss; \
		oss << message; \
		throw monero_seed_exception(oss.str()); } \
	while(false)

constexpr std::time_t epoch = 1590969600; //1st June 2020
constexpr std::time_t time_step = 2629746; //30.436875 days = 1/12 of the Gregorian year

constexpr unsigned date_bits = 10;
constexpr unsigned date_mask = (1u << date_bits) - 1;
constexpr unsigned version_bits = 3;
constexpr unsigned version_mask = (1u << version_bits) - 1;
constexpr unsigned reserved_bits = 2;
constexpr unsigned reserved_mask = (1u << reserved_bits) - 1;
constexpr unsigned check_digits = 1;
constexpr unsigned checksum_size = gf_elem::size() * check_digits;
constexpr unsigned phrase_words = gf_poly::max_degree + 1;
constexpr unsigned total_bits = gf_elem::size() * phrase_words;
constexpr uint32_t argon_tcost = 3;
constexpr uint32_t argon_mcost = 256 * 1024;

static_assert(total_bits
	== version_bits + date_bits + reserved_bits + checksum_size +
	sizeof(monero_seed::secret_seed) * CHAR_BIT,
	"Invalid mnemonic seed size");

static void write_data(gf_poly& poly, unsigned& rem_bits, unsigned value, unsigned bits) {
	if (rem_bits == 0) {
		poly.set_degree(poly.degree() + 1);
		rem_bits = gf_elem::size();
	}
	unsigned digit_bits = std::min(rem_bits, bits);
	unsigned rest_bits = bits - digit_bits;
	rem_bits -= digit_bits;
	poly[poly.degree()] |= ((value >> rest_bits) & ((1u << digit_bits) - 1)) << rem_bits;
	if (rest_bits > 0) {
		write_data(poly, rem_bits, value & ((1u << rest_bits) - 1), rest_bits);
	}
}

template<typename T>
static void read_data(gf_poly& poly, unsigned& used_bits, T& value, unsigned bits) {
	unsigned coeff_index = used_bits / gf_elem::size();
	unsigned bit_index = used_bits % gf_elem::size();
	unsigned digit_bits = std::min((unsigned)gf_elem::size() - bit_index, bits);
	unsigned rem_bits = gf_elem::size() - bit_index - digit_bits;
	unsigned rest_bits = bits - digit_bits;
	value |= ((poly[coeff_index].value() >> rem_bits) & ((1u << bits) - 1)) << rest_bits;
	used_bits += digit_bits;
	if (rest_bits > 0) {
		read_data(poly, used_bits, value, rest_bits);
	}
}

static const reed_solomon_code rs(check_digits);

monero_seed::monero_seed(std::time_t date_created) {
	if (date_created < epoch) {
		THROW_EXCEPTION("date_created must not be before 1st June 2020");
	}
	unsigned quantized_date = ((date_created - epoch) / time_step) & date_mask;
	date_ = epoch + quantized_date * time_step;
	version_ = 0;
	reserved_ = 0;
	secure_random::gen_bytes(seed_.data(), seed_.size());
	
	char salt[25] = "Monero 14-word seed";
	salt[20] = version_;
	store32(salt + 21, quantized_date);
	argon2id_hash_raw(argon_tcost, argon_mcost, 1, seed_.data(), seed_.size(), salt, sizeof(salt), key_.data(), key_.size());

	unsigned rem_bits = gf_elem::size();
	write_data(message_, rem_bits, version_, version_bits);
	write_data(message_, rem_bits, reserved_, reserved_bits);
	write_data(message_, rem_bits, quantized_date, date_bits);
	for (auto byte : seed_) {
		write_data(message_, rem_bits, byte, CHAR_BIT);
	}
	assert(rem_bits == 0);
	rs.encode(message_);
}

monero_seed::monero_seed(const std::string& phrase) {
	int word_count = 0;
	size_t offset = 0;
	int error = -1;
	std::string words[phrase_words];
	do {
		size_t delim = phrase.find(' ', offset);
		if (delim == std::string::npos) {
			delim = phrase.size();
		}
		words[word_count] = phrase.substr(offset, delim - offset);
		auto index = wordlist::english.parse(words[word_count]);
		if (index == -1) {
			if (words[word_count] != erasure) {
				THROW_EXCEPTION("unrecognized word: '" << words[word_count] << "'");
			}
			if (error >= 0) {
				THROW_EXCEPTION("teo or more erasures cannot be corrected");
			}
			error = word_count;
		}
		message_[word_count] = index;
		word_count++;
		offset = delim + 1;
	} while (offset < phrase.size());

	if (word_count != phrase_words) {
		THROW_EXCEPTION("the mnemonic phrase must consist of " << phrase_words << " words");
	}

	message_.set_degree();

	if (error >= 0) {
		for (unsigned i = 0; i < gf_2048::elements(); ++i) {
			message_[error] = i;
			if (rs.check(message_)) {
				correction_ = wordlist::english.get_word(i);
				break;
			}
		}
		assert(!correction_.empty());
	}
	else {
		if (!rs.check(message_)) {
			THROW_EXCEPTION("phrase is invalid (checksum mismatch)");
		}
	}

	unsigned used_bits = checksum_size;
	unsigned quantized_date;

	version_ = 0;
	reserved_ = 0;
	quantized_date = 0;
	memset(seed_.data(), 0, seed_.size());

	read_data(message_, used_bits, version_, version_bits);
	read_data(message_, used_bits, reserved_, reserved_bits);
	read_data(message_, used_bits, quantized_date, date_bits);

	for (uint8_t& byte : seed_) {
		read_data(message_, used_bits, byte, CHAR_BIT);
	}

	assert(used_bits == total_bits);

	date_ = epoch + quantized_date * time_step;

	char salt[25] = "Monero 14-word seed";
	salt[20] = version_;
	store32(salt + 21, quantized_date);
	argon2id_hash_raw(argon_tcost, argon_mcost, 1, seed_.data(), seed_.size(), salt, sizeof(salt), key_.data(), key_.size());
}

std::ostream& operator<<(std::ostream& os, const monero_seed& seed) {
	for (int i = 0; i <= seed.message_.degree(); ++i) {
		if (i > 0) {
			os << " ";
		}
		os << wordlist::english.get_word(seed.message_[i].value());
	}
	return os;
}

std::ostream& operator<<(std::ostream& os, const monero_seed::secret_key& key) {
	os << std::hex;
	for (int i = 0; i < key.size(); ++i) {
		os << std::setw(2) << std::setfill('0') << (unsigned)key[i];
	}
	os << std::dec;
	return os;
}
