# Changelog

All notable changes to XPoint are documented in this file.

## [Unreleased]

### Fixed

- SD-card firmware update and other confirmation dialogs: pressing Confirm was treated as Cancel after the defensive pop-result guard introduced in #75. Confirm again starts the update (this also unblocks locked X4 Pro devices downgrading to upstream CrossPoint firmware from the SD card).
