// ADisplay —— 拿到 WinRT IBuffer 底层指针的规范做法
//
// WriteableBitmap.PixelBuffer 是 WinRT 的 IBuffer，它只暴露字节流，不给裸指针；
// 要直接往里拷像素就得转成这个 COM 接口。GUID 是它公开的定义。
//
// 与 MirrorAudioPlayer 里那个 IMemoryBufferByteAccess 不是同一个接口：那个对应
// IMemoryBufferReference（AudioFrame 的缓冲），这个对应 IBuffer。
//
// 用法上有个坑：CsWinRT 下**不能**用 (IBufferByteAccess)buffer 这种 C 风格的强转 ——
// 投影对象不是普通 COM 对象，强转会抛 InvalidCastException（Invalid cast from
// 'WinRT.IInspectable'）。必须走 CsWinRT 的 buffer.As<IBufferByteAccess>()。

using System.Runtime.InteropServices;

namespace ADisplay.Windows.Interop;

[ComImport]
[Guid("905a0fef-bc53-11df-8c49-001e4fc686da")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal unsafe interface IBufferByteAccess
{
    void Buffer(out byte* buffer);
}
