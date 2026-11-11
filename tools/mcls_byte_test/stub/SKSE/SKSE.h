#pragma once
// tools/mcls_byte_test/stub/SKSE/SKSE.h -- HOST STAND-IN: an in-memory SKSE::SerializationInterface with the method
// names and return shapes classes/State.cpp uses (OpenRecord, WriteRecordData, ReadRecordData, ResolveFormID). The
// test fills `in` with one record body and reads `written` back. See stub/RE/Skyrim.h.
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "RE/Skyrim.h"

namespace SKSE {
    class SerializationInterface {
    public:
        struct Record {
            std::uint32_t type = 0, version = 0;
            std::vector<std::uint8_t> data;
        };
        std::vector<Record>       written;   // every record a CoSave opened
        std::vector<std::uint8_t> in;        // the body of the record a CoLoad reads
        std::size_t               pos = 0;
        std::function<bool(RE::FormID, RE::FormID&)> resolver;   // empty = identity

        bool OpenRecord(std::uint32_t a_type, std::uint32_t a_version) {
            written.push_back({ a_type, a_version, {} });
            return true;
        }
        bool WriteRecordData(const void* a_buf, std::uint32_t a_length) {
            if (written.empty()) return false;
            const auto* p = static_cast<const std::uint8_t*>(a_buf);
            written.back().data.insert(written.back().data.end(), p, p + a_length);
            return true;
        }
        template <class T, std::enable_if_t<std::negation_v<std::is_pointer<T>>, int> = 0>
        bool WriteRecordData(const T& a_v) { return WriteRecordData(std::addressof(a_v), static_cast<std::uint32_t>(sizeof(T))); }

        std::uint32_t ReadRecordData(void* a_buf, std::uint32_t a_length) {
            const std::size_t left = pos < in.size() ? in.size() - pos : 0;
            const auto n = static_cast<std::uint32_t>(left < a_length ? left : a_length);
            if (n && a_buf) std::memcpy(a_buf, in.data() + pos, n);
            pos += n;
            return n;
        }
        template <class T, std::enable_if_t<std::negation_v<std::is_pointer<T>>, int> = 0>
        std::uint32_t ReadRecordData(T& a_v) { return ReadRecordData(std::addressof(a_v), static_cast<std::uint32_t>(sizeof(T))); }

        bool ResolveFormID(RE::FormID a_old, RE::FormID& a_new) const {
            if (resolver) return resolver(a_old, a_new);
            a_new = a_old;
            return true;
        }
    };
}
