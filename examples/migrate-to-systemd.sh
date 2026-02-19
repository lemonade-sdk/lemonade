#!/bin/bash

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if running as root
if [[ $EUID -ne 0 ]]; then
   echo -e "${RED}Error: This script must be run as root${NC}"
   echo "Please run with: sudo $0"
   exit 1
fi

echo "Lemonade Migration Script"
echo "========================="
echo "This script migrates user data from a user account to the lemonade systemd service user."
echo ""

# Prompt for source user
read -p "Enter the username to migrate data from: " SOURCE_USER

# Validate source user exists
if ! id "$SOURCE_USER" &>/dev/null; then
    echo -e "${RED}Error: User '$SOURCE_USER' does not exist${NC}"
    exit 1
fi

# Get source user's home directory
SOURCE_HOME=$(getent passwd "$SOURCE_USER" | cut -d: -f6)
if [[ -z "$SOURCE_HOME" ]]; then
    echo -e "${RED}Error: Could not determine home directory for user '$SOURCE_USER'${NC}"
    exit 1
fi
if [[ ! -d "$SOURCE_HOME" ]]; then
    echo -e "${RED}Error: Home directory '$SOURCE_HOME' does not exist${NC}"
    exit 1
fi

# Check if lemonade user exists
if ! id "lemonade" &>/dev/null; then
    echo -e "${RED}Error: 'lemonade' user does not exist${NC}"
    echo "The systemd service user must be created before running this migration."
    exit 1
fi

# Get lemonade user's home directory
LEMONADE_HOME=$(getent passwd "lemonade" | cut -d: -f6)
if [[ -z "$LEMONADE_HOME" ]]; then
    echo -e "${RED}Error: Could not determine home directory for user 'lemonade'${NC}"
    exit 1
fi
if [[ ! -d "$LEMONADE_HOME" ]]; then
    echo -e "${RED}Error: Lemonade home directory '$LEMONADE_HOME' does not exist${NC}"
    exit 1
fi

# Define paths to migrate
declare -a SOURCE_PATHS=(
    "$SOURCE_HOME/.cache/lemonade"
    "$SOURCE_HOME/.cache/huggingface"
    "$SOURCE_HOME/.local/share/lemonade-server"
)

declare -a DEST_PATHS=(
    "$LEMONADE_HOME/.cache/lemonade"
    "$LEMONADE_HOME/.cache/huggingface"
    "$LEMONADE_HOME/.local/share/lemonade-server"
)

# Check which files exist
echo ""
echo "Scanning for files to migrate..."
echo ""

declare -a FILES_TO_MIGRATE=()
declare -a DEST_FILES=()

for i in "${!SOURCE_PATHS[@]}"; do
    SOURCE_PATH="${SOURCE_PATHS[$i]}"
    DEST_PATH="${DEST_PATHS[$i]}"

    if [[ -e "$SOURCE_PATH" ]]; then
        # Calculate size
        SIZE=$(du -sh "$SOURCE_PATH" 2>/dev/null | cut -f1)
        echo -e "${GREEN}Found:${NC} $SOURCE_PATH (${SIZE})"
        echo -e "  -> Will migrate to: $DEST_PATH"
        FILES_TO_MIGRATE+=("$SOURCE_PATH")
        DEST_FILES+=("$DEST_PATH")
    fi
done

if [[ ${#FILES_TO_MIGRATE[@]} -eq 0 ]]; then
    echo -e "${YELLOW}No files found to migrate.${NC}"
    echo "The user may not have any lemonade data, or it may be in a custom location."
    exit 0
fi

echo ""
echo "Summary:"
echo "--------"
echo "Source user: $SOURCE_USER ($SOURCE_HOME)"
echo "Destination user: lemonade ($LEMONADE_HOME)"
echo "Files/directories to migrate: ${#FILES_TO_MIGRATE[@]}"
echo ""

# Confirm before proceeding
read -p "Do you want to proceed with the migration? (yes/no): " CONFIRM

if [[ "$CONFIRM" != "yes" ]]; then
    echo "Migration cancelled."
    exit 0
fi

echo ""
echo "Starting migration..."

# Perform migration
for i in "${!FILES_TO_MIGRATE[@]}"; do
    SOURCE_PATH="${FILES_TO_MIGRATE[$i]}"
    DEST_PATH="${DEST_FILES[$i]}"

    echo ""
    echo -e "${GREEN}Migrating:${NC} $SOURCE_PATH"
    echo -e "       to: $DEST_PATH"

    # Create parent directory if needed
    PARENT_DIR=$(dirname "$DEST_PATH")
    if [[ ! -d "$PARENT_DIR" ]]; then
        echo "  Creating parent directory: $PARENT_DIR"
        mkdir -p "$PARENT_DIR"
        chown lemonade:lemonade "$PARENT_DIR"
    fi

    # Check if destination already exists
    if [[ -e "$DEST_PATH" ]]; then
        echo -e "  ${YELLOW}Warning: Destination already exists - merging files${NC}"
        # Use rsync to merge directories (move source files into destination)
        echo "  Merging into existing destination..."
        rsync -av --remove-source-files "$SOURCE_PATH/" "$DEST_PATH/"
    else
        # Move files (preserves source structure)
        echo "  Moving files..."
        mv "$SOURCE_PATH" "$DEST_PATH"
    fi

    # Update ownership
    echo "  Updating ownership to lemonade:lemonade..."
    chown -R lemonade:lemonade "$DEST_PATH"

    echo -e "  ${GREEN}Done${NC}"
done

echo ""
echo -e "${GREEN}Migration completed successfully!${NC}"
echo ""
echo "Next steps:"
echo "1. Verify the migrated files in $LEMONADE_HOME"
echo "2. (Re)start the lemonade systemd service: systemctl restart lemonade-server"
echo "3. Check service status: systemctl status lemonade-server"
echo "4. Optionally, remove old files from $SOURCE_HOME to free up space"
echo ""
