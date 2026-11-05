# Test Sample Policy

Format sample inputs must come from sources independent of Peare and its test
code. Do not author sample bytes by hand. Do not write a project script that
constructs a sample file from format rules or byte layouts. Do not mutate,
truncate, or corrupt a sample in a test to create another sample.

An independent program can create a small sample. For example, WinRAR can create
a RAR archive from project-owned text files. The sample remains synthetic, but
its format structure comes from a separate implementation. Record the program,
version, operation, source files, and settings. A project test script is not an
independent program, even when it uses a standard library or writes valid bytes.

A real sample can be committed only when its license allows redistribution.
Record its source and license in a fixture README and in the sample provenance
index. Do not commit proprietary disk images, firmware, ripped media, or other
restricted files.

Writer output may be the subject of a Writer test. Verify that output with an
independent reader or tool. Do not use a file written by Peare Writer as evidence
that Peare Opener can read that format correctly. For Opener tests, use a real
sample or a sample produced by an independent program.

Test payloads are not format samples. Tests may create payload files for a
Writer operation, but must not wrap them into a format sample with test code.

If no independent sample is available, do not invent one. Record the format as
not verified with an independent sample. Test local or restricted samples
outside the repository when permission allows.

Record the origin of every repository fixture in its fixture README. Do not
infer a source, license, tool, or version. Mark missing data as unknown.
