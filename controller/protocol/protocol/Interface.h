#pragma once
#include "../../../tools/Buffers.h"
#include <memory>
#include <type_traits>

namespace tgvoip
{
struct VersionInfo;

template <typename T>
struct SingleChoice
{
    static std::shared_ptr<T> choose(const BufferInputStream &in, const VersionInfo &ver)
    {
        return std::make_shared<T>();
    }

    uint8_t getID() const
    {
        return 0;
    }

    static const uint8_t ID = 0;
};

template <typename T>
struct MultiChoice
{
private:
    static std::shared_ptr<T> __forceChoice(const BufferInputStream &in, const VersionInfo &ver)
    {
        return T::choose(in, ver);
    }

public:
    virtual uint8_t getID() const = 0;

    virtual size_t getConstructorSize(const VersionInfo &ver) const = 0;
};

template <class T, class = void>
struct has_flags : std::false_type
{
};

template <class T>
struct has_flags<T, std::void_t<typename T::Flags>> : std::true_type
{
};

struct Empty
{
    enum Flags : uint8_t
    {
    };
};

template <typename T>
struct Constructor // : public T
{
    uint8_t getID() const
    {
        return T::ID;
    }
    using Flags = typename std::conditional<tgvoip::has_flags<T>::value, T, Empty>::type::Flags;
};

struct Serializable
{
public:
    virtual bool parse(const BufferInputStream &in, const VersionInfo &ver) = 0;
    virtual void serialize(BufferOutputStream &out, const VersionInfo &ver) const = 0;

    virtual std::string print() const = 0;
    virtual size_t getSize(const VersionInfo &ver) const = 0;
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
        for (uint8_t i = 0; i < 8; i++)
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
        for (uint8_t i = 0; i < 8; i++)
        {
            if (v[i])
            {
                mask |= 1 << i;
            }
        }
        out.WriteByte(mask);
        for (const auto &data : v)
        {
            if (data)
                out.Write(data, ver);
        }
    }
    auto begin() noexcept
    {
        return v.begin();
    }
    auto end() noexcept
    {
        return v.end();
    }
    const auto begin() const noexcept
    {
        return v.begin();
    }
    const auto end() const noexcept
    {
        return v.end();
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
    std::string print() const override
    {
        std::string res = "|";
        for (const auto &data : v)
        {
            res += (data ? data.print() : "");
            res += "|";
        }
        return res;
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        size_t res = 1;
        for (const auto &data : v)
        {
            if (data)
                res += data.getSize(ver);
        }
        return res;
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
    auto begin() noexcept
    {
        return v.begin();
    }
    auto end() noexcept
    {
        return v.end();
    }
    const auto begin() const noexcept
    {
        return v.begin();
    }
    const auto end() const noexcept
    {
        return v.end();
    }
    operator bool() const
    {
        return !v.empty();
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        size_t res = 1;
        for (const auto &data : v)
        {
            res += data.getSize(ver);
        }
        return res;
    }
    std::string print() const override
    {
        std::string res;
        for (const auto &val : v)
        {
            res += val.print();
            res += ", ";
        }
        return res;
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
        return d->parse(in.GetPartBuffer(len), ver);
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

    uint8_t getID() const
    {
        return d->getID();
    }

    template <typename X>
    X &get()
    {
        return *dynamic_cast<X>(d.get());
    }

    template <typename X>
    const X &get() const
    {
        return *dynamic_cast<X>(d.get());
    }

    template <typename X>
    operator X &()
    {
        return *dynamic_cast<X>(d.get());
    }

    template <typename X>
    operator const X &() const
    {
        return *dynamic_cast<X>(d.get());
    }

    std::string print() const override
    {
        return d->print();
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        return 1 + d->getSize(ver);
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
    std::string print() const override
    {
        return "buffer";
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        return data.Length();
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
    std::string print() const override
    {
        return std::to_string(data);
    }

    size_t getSize(const VersionInfo &ver) const override
    {
        return 4;
    }
    uint32_t data;
};

namespace Template
{
template <class T>
struct Constructor : public T, tgvoip::Constructor<T>
{
};
}; // namespace Template
} // namespace tgvoip