#pragma once

#include "lemon/jobs/job_types.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace lemon {
namespace jobs {

namespace expr_detail {

inline json resolve_ref_path(const std::string& path, const json& ctx) {
    const json* cur = &ctx;
    size_t start = 0;
    while (start <= path.size()) {
        size_t dot = path.find('.', start);
        std::string seg = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (cur->is_object() && cur->contains(seg)) {
            cur = &(*cur)[seg];
        } else if (cur->is_array()) {
            char* end = nullptr;
            long idx = std::strtol(seg.c_str(), &end, 10);
            if (end == seg.c_str() || *end != '\0' || idx < 0 || (size_t)idx >= cur->size())
                throw JobError(400, "unknown reference: ${" + path + "}");
            cur = &(*cur)[(size_t)idx];
        } else {
            throw JobError(400, "unknown reference: ${" + path + "}");
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return *cur;
}

inline std::string stringify(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return std::to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_null()) return "";
    return v.dump();
}

inline bool truthy(const json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    if (v.is_null()) return false;
    return !v.empty();
}

enum class Tok { Num, Str, Ref, True, False, Null, Op, End };

struct Token {
    Tok kind;
    std::string text;
    double num = 0;
};

inline std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0;
    auto starts = [&](const char* op) {
        return s.compare(i, std::char_traits<char>::length(op), op) == 0;
    };
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace((unsigned char)c)) { i++; continue; }
        if (starts("${")) {
            size_t close = s.find('}', i + 2);
            if (close == std::string::npos) throw JobError(400, "unterminated ${ in expression");
            out.push_back({Tok::Ref, s.substr(i + 2, close - (i + 2)), 0});
            i = close + 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            size_t j = i + 1;
            std::string lit;
            while (j < s.size() && s[j] != q) {
                if (s[j] == '\\' && j + 1 < s.size()) { lit.push_back(s[j + 1]); j += 2; }
                else lit.push_back(s[j++]);
            }
            if (j >= s.size()) throw JobError(400, "unterminated string in expression");
            out.push_back({Tok::Str, lit, 0});
            i = j + 1;
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            char* end = nullptr;
            double v = std::strtod(s.c_str() + i, &end);
            out.push_back({Tok::Num, "", v});
            i = end - s.c_str();
            continue;
        }
        if (std::isalpha((unsigned char)c)) {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            std::string word = s.substr(i, j - i);
            if (word == "true") out.push_back({Tok::True, word, 0});
            else if (word == "false") out.push_back({Tok::False, word, 0});
            else if (word == "null") out.push_back({Tok::Null, word, 0});
            else throw JobError(400, "unexpected identifier '" + word + "' in expression");
            i = j;
            continue;
        }
        for (const char* op : {"&&", "||", "==", "!=", "<=", ">="}) {
            if (starts(op)) { out.push_back({Tok::Op, op, 0}); i += 2; goto next; }
        }
        if (std::string("!<>+-*/()").find(c) != std::string::npos) {
            out.push_back({Tok::Op, std::string(1, c), 0});
            i++;
            continue;
        }
        throw JobError(400, std::string("unexpected character '") + c + "' in expression");
    next:;
    }
    out.push_back({Tok::End, "", 0});
    return out;
}

class Parser {
public:
    Parser(std::vector<Token> toks, const json& ctx) : toks_(std::move(toks)), ctx_(ctx) {}

    json parse() {
        json v = parse_or();
        if (peek().kind != Tok::End) throw JobError(400, "trailing tokens in expression");
        return v;
    }

    void set_syntax_only() { syntax_only_ = true; }

private:
    const Token& peek() const { return toks_[pos_]; }
    const Token& advance() { return toks_[pos_++]; }
    bool accept_op(const char* op) {
        if (peek().kind == Tok::Op && peek().text == op) { pos_++; return true; }
        return false;
    }

    double num(const json& v, const char* ctx) const {
        if (!v.is_number()) {
            if (syntax_only_) return 0.0;
            throw JobError(400, std::string("expected a number for ") + ctx);
        }
        return v.get<double>();
    }

    json parse_or() {
        json l = parse_and();
        while (accept_op("||")) { json r = parse_and(); l = truthy(l) || truthy(r); }
        return l;
    }
    json parse_and() {
        json l = parse_not();
        while (accept_op("&&")) { json r = parse_not(); l = truthy(l) && truthy(r); }
        return l;
    }
    json parse_not() {
        if (accept_op("!")) return !truthy(parse_not());
        return parse_cmp();
    }
    json parse_cmp() {
        json l = parse_add();
        for (const char* op : {"==", "!=", "<=", ">=", "<", ">"}) {
            if (peek().kind == Tok::Op && peek().text == op) {
                advance();
                json r = parse_add();
                return compare(op, l, r);
            }
        }
        return l;
    }
    json parse_add() {
        json l = parse_mul();
        while (peek().kind == Tok::Op && (peek().text == "+" || peek().text == "-")) {
            std::string op = advance().text;
            json r = parse_mul();
            l = op == "+" ? num(l, "+") + num(r, "+") : num(l, "-") - num(r, "-");
        }
        return l;
    }
    json parse_mul() {
        json l = parse_unary();
        while (peek().kind == Tok::Op && (peek().text == "*" || peek().text == "/")) {
            std::string op = advance().text;
            json r = parse_unary();
            if (op == "*") l = num(l, "*") * num(r, "*");
            else {
                double d = num(r, "/");
                if (d == 0.0) {
                    if (syntax_only_) { l = 0.0; continue; }
                    throw JobError(400, "division by zero in expression");
                }
                l = num(l, "/") / d;
            }
        }
        return l;
    }
    json parse_unary() {
        if (accept_op("-")) return -num(parse_unary(), "unary -");
        return parse_primary();
    }
    json parse_primary() {
        const Token& t = peek();
        switch (t.kind) {
            case Tok::Num: advance(); return t.num;
            case Tok::Str: advance(); return t.text;
            case Tok::True: advance(); return true;
            case Tok::False: advance(); return false;
            case Tok::Null: advance(); return json(nullptr);
            case Tok::Ref:
                advance();
                if (syntax_only_) return json(nullptr);
                return resolve_ref_path(t.text, ctx_);
            case Tok::Op:
                if (t.text == "(") {
                    advance();
                    json v = parse_or();
                    if (!accept_op(")")) throw JobError(400, "missing ) in expression");
                    return v;
                }
                break;
            default: break;
        }
        throw JobError(400, "unexpected token in expression");
    }

    json compare(const std::string& op, const json& l, const json& r) const {
        if (op == "==") return l == r;
        if (op == "!=") return l != r;
        if (l.is_number() && r.is_number()) {
            double a = l.get<double>(), b = r.get<double>();
            if (op == "<") return a < b;
            if (op == "<=") return a <= b;
            if (op == ">") return a > b;
            return a >= b;
        }
        if (l.is_string() && r.is_string()) {
            const auto& a = l.get_ref<const std::string&>();
            const auto& b = r.get_ref<const std::string&>();
            if (op == "<") return a < b;
            if (op == "<=") return a <= b;
            if (op == ">") return a > b;
            return a >= b;
        }
        if (syntax_only_) return false;
        throw JobError(400, "cannot order-compare these operand types");
    }

    std::vector<Token> toks_;
    const json& ctx_;
    size_t pos_ = 0;
    bool syntax_only_ = false;
};

}

inline json resolve_refs(const json& value, const json& ctx) {
    if (value.is_object()) {
        json out = json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            out[it.key()] = resolve_refs(it.value(), ctx);
        return out;
    }
    if (value.is_array()) {
        json out = json::array();
        for (const auto& e : value) out.push_back(resolve_refs(e, ctx));
        return out;
    }
    if (!value.is_string()) return value;

    const std::string s = value.get<std::string>();
    const size_t open = s.find("${");
    if (open == std::string::npos) return value;
    const size_t close = s.find('}', open + 2);
    if (close == std::string::npos) return value;

    if (open == 0 && close == s.size() - 1 && s.find("${", 2) == std::string::npos)
        return expr_detail::resolve_ref_path(s.substr(2, close - 2), ctx);

    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t o = s.find("${", i);
        if (o == std::string::npos) { out += s.substr(i); break; }
        out += s.substr(i, o - i);
        size_t c = s.find('}', o + 2);
        if (c == std::string::npos) { out += s.substr(o); break; }
        out += expr_detail::stringify(expr_detail::resolve_ref_path(s.substr(o + 2, c - (o + 2)), ctx));
        i = c + 1;
    }
    return out;
}

inline bool eval_condition(const std::string& expr, const json& ctx) {
    if (expr.empty()) return true;
    return expr_detail::truthy(expr_detail::Parser(expr_detail::tokenize(expr), ctx).parse());
}

inline std::string check_expression_syntax(const std::string& expr) {
    if (expr.empty()) return "";
    static const json placeholder = json(nullptr);
    try {
        expr_detail::Parser parser(expr_detail::tokenize(expr), placeholder);
        parser.set_syntax_only();
        parser.parse();
    } catch (const JobError& e) {
        return e.what();
    }
    return "";
}

}
}
