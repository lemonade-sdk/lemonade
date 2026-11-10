"""Keep the legacy missing-reference fault from corrupting valid builtin references at initialization."""

import container_definition_mutations as campaign

ORIGINAL = campaign.mutations


def mutations(schema):
    result = []
    for name, filename, change, assertion in ORIGINAL(schema):
        if name == "container-reference":
            change = (
                change[0],
                'const auto name = policies.contains(item["container"].get<std::string>()) ? item["container"].get<std::string>() : std::string("rocm");',
            )
        result.append((name, filename, change, assertion))
    return result


if __name__ == "__main__":
    campaign.mutations = mutations
    campaign.main()
