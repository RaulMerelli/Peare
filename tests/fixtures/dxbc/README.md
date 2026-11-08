# DXBC Fixture

`postprocess-copy.cso` is a DXBC shader compiled by Microsoft FXC 10.1,
FileVersion 10.0.26100.8249, from Microsoft DirectXTK's `PostProcess.fx`.
The source was downloaded from commit
[`da217971ae7bf2293e13a65ddfae963d02c129ff`](https://github.com/microsoft/DirectXTK/commit/da217971ae7bf2293e13a65ddfae963d02c129ff)
under `Src/Shaders/PostProcess.fx`; `Structures.fxh` was downloaded from the
same commit. DirectXTK is distributed under the MIT License.

Command:

```text
fxc.exe /T ps_4_0 /E PSCopy /I <shader-directory> /Fo postprocess-copy.cso PostProcess.fx
```

FXC `/dumpbin` reports the resource bindings and input/output signatures. The
fixture SHA-256 is `5d3abdb41d6ef3452662d22c4781024eb89f1a14ff9fb6fff822e0a0088df7bf`.
