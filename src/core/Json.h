#ifndef _JSON_H_
#define _JSON_H_

#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Minimal JSON reader (objects, arrays, strings, numbers, true/false/null).
// Enough for the engine's data files; not a general purpose parser (no \u surrogates).
struct Json
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool Has(const std::string& key) const { return type == Type::Object && object.count(key) > 0; }
    const Json* Find(const std::string& key) const
    {
        if (type != Type::Object) return nullptr;
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
    int Int(int fallback = 0) const { return type == Type::Number ? static_cast<int>(number) : fallback; }

    static Json Parse(const std::string& text)
    {
        std::size_t pos = 0;
        Json v = ParseValue(text, pos);
        return v;
    }

private:
    static void SkipSpace(const std::string& t, std::size_t& p)
    {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p;
    }

    static std::string ParseString(const std::string& t, std::size_t& p)
    {
        std::string s;
        ++p;   // opening quote
        while (p < t.size() && t[p] != '"')
        {
            if (t[p] == '\\' && p + 1 < t.size())
            {
                ++p;
                switch (t[p])
                {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u':
                {
                    // Only the BMP, encoded as UTF-8.
                    unsigned code = 0;
                    for (int k = 1; k <= 4 && p + k < t.size(); ++k)
                        code = code * 16 + static_cast<unsigned>(std::strtol(std::string(1, t[p + k]).c_str(), nullptr, 16));
                    p += 4;
                    if (code < 0x80) s += static_cast<char>(code);
                    else if (code < 0x800) { s += static_cast<char>(0xC0 | (code >> 6)); s += static_cast<char>(0x80 | (code & 0x3F)); }
                    else { s += static_cast<char>(0xE0 | (code >> 12)); s += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); s += static_cast<char>(0x80 | (code & 0x3F)); }
                    break;
                }
                default: s += t[p]; break;
                }
            }
            else
            {
                s += t[p];
            }
            ++p;
        }
        ++p;   // closing quote
        return s;
    }

    static Json ParseValue(const std::string& t, std::size_t& p)
    {
        SkipSpace(t, p);
        if (p >= t.size()) throw std::runtime_error("JSON: unexpected end");
        Json v;
        const char c = t[p];
        if (c == '{')
        {
            v.type = Type::Object;
            ++p;
            SkipSpace(t, p);
            if (t[p] == '}') { ++p; return v; }
            while (true)
            {
                SkipSpace(t, p);
                if (t[p] != '"') throw std::runtime_error("JSON: expected key");
                std::string key = ParseString(t, p);
                SkipSpace(t, p);
                if (t[p] != ':') throw std::runtime_error("JSON: expected ':'");
                ++p;
                v.object.emplace(std::move(key), ParseValue(t, p));
                SkipSpace(t, p);
                if (t[p] == ',') { ++p; continue; }
                if (t[p] == '}') { ++p; break; }
                throw std::runtime_error("JSON: expected ',' or '}'");
            }
        }
        else if (c == '[')
        {
            v.type = Type::Array;
            ++p;
            SkipSpace(t, p);
            if (t[p] == ']') { ++p; return v; }
            while (true)
            {
                v.array.push_back(ParseValue(t, p));
                SkipSpace(t, p);
                if (t[p] == ',') { ++p; continue; }
                if (t[p] == ']') { ++p; break; }
                throw std::runtime_error("JSON: expected ',' or ']'");
            }
        }
        else if (c == '"')
        {
            v.type = Type::String;
            v.string = ParseString(t, p);
        }
        else if (t.compare(p, 4, "true") == 0) { v.type = Type::Bool; v.boolean = true; p += 4; }
        else if (t.compare(p, 5, "false") == 0) { v.type = Type::Bool; v.boolean = false; p += 5; }
        else if (t.compare(p, 4, "null") == 0) { p += 4; }
        else
        {
            char* end = nullptr;
            v.number = std::strtod(t.c_str() + p, &end);
            if (end == t.c_str() + p) throw std::runtime_error("JSON: bad value");
            v.type = Type::Number;
            p = static_cast<std::size_t>(end - t.c_str());
        }
        return v;
    }
};

#endif // _JSON_H_
