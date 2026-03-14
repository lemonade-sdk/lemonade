find . -type f -name '*.cpp' -exec sed -i '' 's/[[:space:]]*$//' {} \+
find . -type f -name '*.h' -exec sed -i '' 's/[[:space:]]*$//' {} \+
find . -type f -name '*.md' -exec sed -i '' 's/[[:space:]]*$//' {} \+
