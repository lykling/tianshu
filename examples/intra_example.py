# Copyright 2026 Pride Leong.
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

"""Simple INTRA transport example: writer and reader in the same process."""


# This is a conceptual example. In Python SDK (Phase 2), it would be:
# import tianshu
# node = tianshu.Node()
# writer = node.create_writer("/test", "RawBytes")
# reader = node.create_reader("/test", callback=on_msg)
# writer.write(b"hello")
# For now, this demonstrates the pattern with a simple in-process queue.


class IntraExample:
    """Demonstrates INTRA transport: same-process zero-copy communication."""

    def __init__(self):
        self.received = []

    def run(self):
        # Simulate writer -> reader in same process
        messages = [b"hello", b"world", b"tianshu"]

        print("=== TIANSHU INTRA Transport Example ===")
        print(f"Sending {len(messages)} messages via INTRA (same process)")
        print()

        for i, msg in enumerate(messages):
            print(f"  [WRITE] seq={i} data={msg.decode()}")
            self.received.append(msg)

        print()
        print(f"  [READ] received {len(self.received)} messages:")
        for i, msg in enumerate(self.received):
            print(f"    seq={i} data={msg.decode()}")

        print()
        print("INTRA transport: zero-copy, no serialization, ~10ns latency.")
        print("(In C++ this is a direct function call, not a queue.)")


if __name__ == "__main__":
    IntraExample().run()
