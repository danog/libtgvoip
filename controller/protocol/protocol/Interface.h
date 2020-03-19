#pragma once
#include "../../../tools/Buffers.h"
#include <memory>

namespace tgvoip
{

struct VersionInfo
{
    VersionInfo(int _peerVersion, int32_t _connectionMaxLayer) : peerVersion(_peerVersion), connectionMaxLayer(_connectionMaxLayer){};
    const int peerVersion = 0;
    const int32_t connectionMaxLayer = 0;

    inline bool isNew() const
    {
        return peerVersion >= PROTOCOL_RELIABLE || connectionMaxLayer >= 110;
    }
};

template <typename T>
struct SingleChoice
{
    static std::shared_ptr<T> choose(const BufferInputStream &in, const VersionInfo &ver)
    {
        return std::make_shared<T>();
    }
};

template <typename T>
struct MultiChoice
{
    static std::shared_ptr<T> choose(const BufferInputStream &in, const VersionInfo &ver);
};

struct Serializable
{
public:
    virtual bool parse(const BufferInputStream &in, const VersionInfo &ver) = 0;
    virtual void serialize(BufferOutputStream &out, const VersionInfo &ver) const = 0;
};

template <typename T>
struct Mask : public Serializable, SingleChoice<Mask<T>>
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        uint8_t mask;
        if (!in.TryRead(mask))
        {
            return false;
        }
        for (auto i = 0; i < sizeof(mask); i++)
        {
            if (!(i & (1 << i)))
                continue;

            if (!in.TryRead(v.at(i), ver))
                return false;
        }
        return true;
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        uint8_t mask = 0;
        for (auto i = 0; i < sizeof(mask); i++)
        {
            if (v[i])
            {
                mask |= 1 << i;
            }
        }
        out.WriteByte(mask);
        for (const auto &data : v)
        {
            out.Write(v, data);
        }
    }
    typename std::array<T, 8>::iterator begin()
    {
        return v.begin();
    }
    typename std::array<T, 8>::iterator end()
    {
        return v.end();
    }
    typename std::array<T, 8>::const_iterator cbegin()
    {
        return v.cbegin();
    }
    typename std::array<T, 8>::const_iterator cend()
    {
        return v.cend();
    }
    operator bool() const
    {
        for (const auto &data : v)
        {
            if (data)
                return true;
        }
        return false;
    }
    std::array<T, 8> v{};
};

template <typename T>
struct Array : public Serializable, SingleChoice<Array<T>>
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        uint8_t extraCount;
        if (!in.TryRead(extraCount))
        {
            return false;
        }
        v.reserve(extraCount);
        for (auto i = 0; i < extraCount; i++)
        {
            T data;
            if (!data.parse(in, ver))
            {
                return false;
            }
            v.push_back(std::move(data));
        }
        return true;
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteByte(v.size());
        for (auto &data : v)
        {
            data.serialize(out, ver);
        }
    }
    typename std::vector<T>::iterator begin()
    {
        return v.begin();
    }
    typename std::vector<T>::iterator end()
    {
        return v.end();
    }
    typename std::vector<T>::const_iterator cbegin()
    {
        return v.cbegin();
    }
    typename std::vector<T>::const_iterator cend()
    {
        return v.cend();
    }
    operator bool() const
    {
        return !v.empty();
    }
    std::vector<T> v;
};

template <class T>
struct Wrapped : public Serializable, SingleChoice<Wrapped<T>>
{
    Wrapped(std::shared_ptr<T> &&_d) : d(_d){};
    Wrapped(std::shared_ptr<T> &_d) : d(_d){};
    Wrapped() = default;

    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        uint8_t len;
        if (!in.TryRead(len))
            return false;
        d = T::choose(in, ver);
        return d->parse(in.GetPartBuffer(len));
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.Advance(1);
        size_t len = out.GetOffset();
        d->serialize(out, ver);
        len = out.GetOffset() - len;
        out.Rewind(len + 1);
        out.WriteByte(len);
        out.Advance(len);
    }
    operator bool() const
    {
        return d && *d;
    }
    std::shared_ptr<T> d;
};

struct Bytes : public Serializable,
               SingleChoice<Bytes>
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver)
    {
        data = Buffer(in.GetLength());
        return in.TryRead(data);
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteBytes(data);
    }
    operator bool()
    {
        return !data.IsEmpty();
    }
    Buffer data;
};
struct UInt32 : public Serializable, SingleChoice<UInt32>
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver)
    {
        return in.TryRead(data);
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteUInt32(data);
    }
    uint32_t data;
};
} // namespace tgvoip