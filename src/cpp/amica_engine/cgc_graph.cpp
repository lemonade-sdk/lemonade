#include "cgc_graph.h"

#include <cstdio>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amica_engine {

namespace {

struct Value {
    std::vector<std::size_t> shape;
    std::string dtype;  // "float32", "float16", "int64"

    std::size_t elements() const {
        std::size_t count = 1;
        for (const std::size_t dimension : shape) count *= dimension;
        return count;
    }

    std::string type() const {
        std::string text = "!cgc.tensor<";
        for (const std::size_t dimension : shape) text += std::to_string(dimension) + "x";
        return text + "!cgc." + dtype + ">";
    }
};

std::string list(const std::vector<std::string>& items) {
    std::string text = "[";
    for (std::size_t i = 0; i < items.size(); ++i) text += (i ? ", " : "") + items[i];
    return text + "]";
}

template <typename T>
std::vector<std::string> integers(const std::vector<T>& values) {
    std::vector<std::string> items;
    for (const T value : values) items.push_back(std::to_string(value));
    return items;
}

// Enough digits to round-trip an f32, always with a decimal point or exponent.
std::string float_literal(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
    std::string result = text;
    if (result.find_first_of(".en") == std::string::npos) result += ".0";
    return result;
}

// Builds one @main_graph entry point, value by value.
class CgcGraph {
public:
    explicit CgcGraph(std::vector<Value> parameters) : parameters_(std::move(parameters)) {
        for (std::size_t i = 0; i < parameters_.size(); ++i) values_["%arg" + std::to_string(i)] = parameters_[i];
    }

    std::string constant(float value) {
        const Value result{{1}, "float32"};
        return emit("cgc.constant(#cgc.dense_float_elements<[" + float_literal(value) + "]> : " + result.type() + ")",
                    result);
    }

    std::string binary(const std::string& operation, const std::string& left, const std::string& right) {
        const Value& a = values_.at(left);
        const Value& b = values_.at(right);
        const Value result = a.elements() >= b.elements() ? a : b;
        return emit("cgc_op." + operation + "(" + left + ", " + right + ") : (" + a.type() + ", " + b.type() +
                        ") -> " + result.type(),
                    result);
    }

    std::string cast(const std::string& source, const std::string& dtype) {
        const Value& from = values_.at(source);
        const Value result{from.shape, dtype};
        return emit("cgc_op.cast(" + source + ") : (" + from.type() + ") -> " + result.type(), result);
    }

    std::string reshape(const std::string& source, const std::vector<std::size_t>& shape) {
        const Value dimensions_value{{shape.size()}, "int64"};
        const std::string dimensions = emit(
            "cgc.constant(#cgc.dense_integer_elements<" + list(integers(shape)) + "> : " + dimensions_value.type() + ")",
            dimensions_value);
        const Value& from = values_.at(source);
        const Value result{shape, from.dtype};
        return emit("cgc_op.reshape(" + source + ", " + dimensions + ") : (" + from.type() + ", " +
                        dimensions_value.type() + ") -> " + result.type(),
                    result);
    }

    std::string transpose(const std::string& source, const std::vector<std::size_t>& permutation) {
        const Value& from = values_.at(source);
        Value result{{}, from.dtype};
        for (const std::size_t axis : permutation) result.shape.push_back(from.shape.at(axis));
        const std::string attribute = "#cgc.dense_integer_elements<" + list(integers(permutation)) +
                                      "> : !cgc.tensor<" + std::to_string(permutation.size()) + "x!cgc.int64>";
        return emit("cgc_op.transpose(" + source + ") {permutation = " + attribute + "} : (" + from.type() + ") -> " +
                        result.type(),
                    result);
    }

    std::string text(const std::vector<std::string>& outputs) const {
        std::vector<std::string> arguments;
        for (std::size_t i = 0; i < parameters_.size(); ++i) {
            arguments.push_back("%arg" + std::to_string(i) + ": " + parameters_[i].type());
        }
        std::string types;
        for (std::size_t i = 0; i < outputs.size(); ++i) types += (i ? ", " : "") + values_.at(outputs[i]).type();
        std::string returned;
        for (std::size_t i = 0; i < outputs.size(); ++i) returned += (i ? ", " : "") + outputs[i];
        std::string arguments_text;
        for (std::size_t i = 0; i < arguments.size(); ++i) arguments_text += (i ? ", " : "") + arguments[i];
        std::string result = "cgc.module {\ncgc.entry_point @main_graph(" + arguments_text + ") -> " +
                             (outputs.size() > 1 ? "(" + types + ")" : types) + " {\n";
        for (const auto& line : lines_) result += line + "\n";
        return result + "cgc.return " + returned + " : " + types + "\n}\n}\n";
    }

private:
    std::string emit(const std::string& expression, const Value& value) {
        const std::string name = "%p" + std::to_string(lines_.size());
        lines_.push_back(name + " = " + expression);
        values_[name] = value;
        return name;
    }

    std::vector<Value> parameters_;
    std::map<std::string, Value> values_;
    std::vector<std::string> lines_;
};

void check(const LatentShape& shape) {
    if (shape.channels == 0 || shape.height == 0 || shape.width == 0 || shape.height % 2 || shape.width % 2) {
        throw std::runtime_error("the latent must have channels and an even height and width");
    }
}

std::vector<std::size_t> spatial(const LatentShape& shape) { return {1, shape.channels, shape.height, shape.width}; }

std::vector<std::size_t> packed(const LatentShape& shape) {
    return {1, shape.height * shape.width / 4, shape.channels * 4};
}

}  // namespace

std::string latent_initial_graph(const LatentShape& shape) {
    check(shape);
    CgcGraph graph({{spatial(shape), "float32"}});
    std::string latent = graph.reshape("%arg0", {1, shape.channels, shape.height / 2, 2, shape.width / 2, 2});
    latent = graph.transpose(latent, {0, 2, 4, 1, 3, 5});
    latent = graph.reshape(latent, packed(shape));
    const std::string half = graph.cast(latent, "float16");
    return graph.text({latent, half});
}

std::string scheduler_step_graph(const LatentShape& shape) {
    check(shape);
    CgcGraph graph({{packed(shape), "float32"}, {packed(shape), "float16"}, {{1}, "float32"}});
    const std::string prediction = graph.cast("%arg1", "float32");
    const std::string change = graph.binary("multiply", prediction, "%arg2");
    const std::string latent = graph.binary("add", "%arg0", change);
    const std::string half = graph.cast(latent, "float16");
    return graph.text({latent, half});
}

std::string decode_input_graph(const LatentShape& shape, float scale_factor, float shift_factor) {
    check(shape);
    CgcGraph graph({{packed(shape), "float32"}});
    std::string latent = graph.reshape("%arg0", {1, shape.height / 2, shape.width / 2, shape.channels, 2, 2});
    latent = graph.transpose(latent, {0, 3, 1, 4, 2, 5});
    latent = graph.reshape(latent, spatial(shape));
    latent = graph.binary("divide", latent, graph.constant(scale_factor));
    latent = graph.binary("add", latent, graph.constant(shift_factor));
    return graph.text({latent});
}

}  // namespace amica_engine
