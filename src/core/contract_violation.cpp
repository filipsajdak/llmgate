#include <contracts>
#include <cstdio>
#include <cstdlib>

void handle_contract_violation(std::contracts::contract_violation const& violation) {
    auto const where = violation.location();
    std::fprintf(stderr, "contract violation: %s at %s:%u in %s\n", violation.comment(),
                 where.file_name(), static_cast<unsigned>(where.line()), where.function_name());
    if (violation.is_terminating()) {
        std::abort();
    }
}
