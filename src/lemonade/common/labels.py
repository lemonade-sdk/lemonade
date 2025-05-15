from typing import Dict, List
import turnkeyml.common.printing as printing


def to_dict(label_list: List[str]) -> Dict[str, List[str]]:
    """
    Convert label list into a dictionary of labels
    """
    label_dict = {}
    for item in label_list:
        try:
            label_key, label_value = item.split("::")
            label_value = label_value.split(",")
            label_dict[label_key] = label_value
        except ValueError:
            printing.log_warning(
                (
                    f"Malformed label {item} found. "
                    "Each label must have the format key::value1,value2,... "
                )
            )
    return label_dict


def load_from_file(file_path: str) -> Dict[str, List[str]]:
    """
    This function extracts labels from a Python file.
    Labels must be in the first line of a Python file and start with "# labels: "
    Each label must have the format "key::value1,value2,..."

    Example:
        "# labels: author::google test_group::daily,monthly"
    """
    # Open file
    with open(file_path, encoding="utf-8") as f:
        first_line = f.readline()

    # Return label dict
    if "# labels:" in first_line:
        label_list = first_line.replace("\n", "").split(" ")[2:]
        return to_dict(label_list)
    else:
        return {}


def is_subset(label_dict_a: Dict[str, List[str]], label_dict_b: Dict[str, List[str]]):
    """
    This function returns True if label_dict_a is a subset of label_dict_b.
    More specifically, we return True if:
        * All keys of label_dict_a are also keys of label_dict_b AND,
        * All values of label_dict_a[key] are values of label_dict_b[key]
    """
    for key in label_dict_a:
        # Skip benchmarking if the label_dict_a key is not a key of label_dict_b
        if key not in label_dict_b:
            return False
        # A label key may point to multiple label values
        # Skip if not all values of label_dict_a[key] are in label_dict_b[key]
        elif not all(elem in label_dict_a[key] for elem in label_dict_b[key]):
            return False
    return True
