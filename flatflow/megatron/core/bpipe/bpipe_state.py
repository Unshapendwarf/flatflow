# Copyright 2025 The FlatFlow Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# BPipe options
_USE_BPIPE = False

def is_bpipe_enabled() -> bool:
    """Return True if bpipe is enabled, False otherwise."""
    return _USE_BPIPE

# BPipe option
def set_bpipe_option(enabled: bool) -> None:
    """Enable or disable bpipe."""
    global _USE_BPIPE
    _USE_BPIPE = enabled
