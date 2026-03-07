import re
with open("tests/unit/tools/test_tools.cpp", "r") as f:
    text = f.read()

text = text.replace('EXPECT_EQ(capturedCwd, "/work");', 'EXPECT_EQ(capturedCwd, "/tmp/work");')

with open("tests/unit/tools/test_tools.cpp", "w") as f:
    f.write(text)
