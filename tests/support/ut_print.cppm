export module llmgate.ut_print;

import std;
import llmgate.format;

export namespace std {

template <llmgate::format::printable_aggregate T>
ostream& operator<<(ostream& out, T const& value) {
    return out << format("{}", value);
}

}  // namespace std
