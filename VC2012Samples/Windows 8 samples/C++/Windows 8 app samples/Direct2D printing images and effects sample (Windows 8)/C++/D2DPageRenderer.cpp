//// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
//// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
//// PARTICULAR PURPOSE.
////
//// Copyright (c) Microsoft Corporation. All rights reserved

#include "pch.h"
#include <math.h>
#include "D2DPageRenderer.h"
#include "D2DPageRendererContext.h"

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::Graphics::Display;

PageRenderer::PageRenderer()
: m_isSnapped(false)
{
}

void PageRenderer::CreateDeviceIndependentResources()
{
    DirectXBase::CreateDeviceIndependentResources();

    // Create a DirectWrite text format object for the sample's content.
    DX::ThrowIfFailed(
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            40.0f,
            L"en-us", // locale
            &m_textFormat
            )
        );

    // Align the text horizontally.
    DX::ThrowIfFailed(
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING)
        );

    // Center the text vertically.
    DX::ThrowIfFailed(
        m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)
        );

    // Create a DirectWrite text format object for status messages.
    DX::ThrowIfFailed(
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            20.0f,
            L"en-us", // locale
            &m_messageFormat
            )
        );

    // Center the text horizontally.
    DX::ThrowIfFailed(
        m_messageFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER)
        );

    // Center the text vertically.
    DX::ThrowIfFailed(
        m_messageFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)
        );

    // Load the source bitmap file into a WIC bitmap source.
    ComPtr<IWICBitmapDecoder> decoder;
    DX::ThrowIfFailed(
        m_wicFactory->CreateDecoderFromFilename(
            L"test.jpg",
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder
            )
        );

    ComPtr<IWICBitmapFrameDecode> frame;
    DX::ThrowIfFailed(
        decoder->GetFrame(0, &frame)
        );

    DX::ThrowIfFailed(
        m_wicFactory->CreateFormatConverter(&m_wicConvertedSource)
        );

    DX::ThrowIfFailed(
        m_wicConvertedSource->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeCustom  // Premultiplied BGRA has no paletting, so this is ignored.
            )
        );

    // Load the source color profile.
    DX::ThrowIfFailed(
        m_wicFactory->CreateColorContext(&m_wicColorContext)
        );

    DX::ThrowIfFailed(
        m_wicColorContext->InitializeFromFilename(L"profile.icm")
        );

    // Create another BitmapSource effect, for printing only.
    // We first encode the WIC bitmap source and the WIC color context to an
    // in-memory stream, then create a new WIC bitmap source from that stream,
    // and finally create a BitmapSource effect based on the newly created WIC
    // bitmap source. By doing so, we pass the original WIC bitmap source and
    // the WIC color context (both embedded in that stream) directly to the
    // print path, and achieve better performance and fidelity with delayed
    // color conversion.

    // Step 1: Encode both the WIC bitmap source and the color context to an
    // in-memory stream.
    ComPtr<IStream> stream;
    DX::ThrowIfFailed(
        CreateStreamOnHGlobal(
            nullptr, // To allocate a new memory handle.
            true,    // The underlying handle should be automatically freed when the stream object is released.
            &stream
            )
        );

    ComPtr<IWICBitmapEncoder> wicBitmapEncoder;
    DX::ThrowIfFailed(
        m_wicFactory->CreateEncoder(
            GUID_ContainerFormatPng,    // Encode as PNG.
            nullptr,    // No preferred codec vendor.
            &wicBitmapEncoder
            )
        );

    DX::ThrowIfFailed(
        wicBitmapEncoder->Initialize(
            stream.Get(),
            WICBitmapEncoderNoCache
            )
        );

    ComPtr<IWICBitmapFrameEncode> wicFrameEncode;
    DX::ThrowIfFailed(
        wicBitmapEncoder->CreateNewFrame(
            &wicFrameEncode,
            nullptr     // No encoder options.
            )
        );

    DX::ThrowIfFailed(
        wicFrameEncode->Initialize(nullptr)
        );

    // Set color context to the frame.
    IWICColorContext* contexts[1];
    contexts[0] = m_wicColorContext.Get();
    DX::ThrowIfFailed(
        wicFrameEncode->SetColorContexts(1, contexts)
        );

    DX::ThrowIfFailed(
        wicFrameEncode->WriteSource(
            m_wicConvertedSource.Get(),
            nullptr     // Write the whole bitmap.
            )
        );

    DX::ThrowIfFailed(
        wicFrameEncode->Commit()
        );

    DX::ThrowIfFailed(
        wicBitmapEncoder->Commit()
        );

    // Step 2: Decode the newly created image stream.
    ComPtr<IWICBitmapDecoder> bitmapDecoder;
    DX::ThrowIfFailed(
        m_wicFactory->CreateDecoderFromStream(
            stream.Get(),
            nullptr,    // No preferred codec vendor.
            WICDecodeMetadataCacheOnLoad,
            &bitmapDecoder
            )
        );

    // Get the initial frame as the new WIC bitmap source.
    DX::ThrowIfFailed(
        bitmapDecoder->GetFrame(0, &m_profiledWicBitmap)
        );
}

void PageRenderer::CreateDeviceResources()
{
    DirectXBase::CreateDeviceResources();

    // Create and initialize sample overlay for sample title.
    m_sampleOverlay = ref new SampleOverlay();
    m_sampleOverlay->Initialize(
        m_d2dDevice.Get(),
        m_d2dContext.Get(),
        m_wicFactory.Get(),
        m_dwriteFactory.Get(),
        "Direct2D image and effect printing sample"
        );

    D2D1_SIZE_F size = m_d2dContext->GetSize();

    // Exclude sample title from imageable area for image drawing.
    float titleHeight = m_sampleOverlay->GetTitleHeightInDips();

    // Create and initialize the page renderer context for display.
    m_displayPageRendererContext =
        ref new PageRendererContext(
            D2D1::RectF(0, titleHeight, size.width, size.height),
            m_d2dContext.Get(),
            DrawTypes::Rendering,
            this
            );
}

void PageRenderer::UpdateForWindowSizeChange()
{
    DirectXBase::UpdateForWindowSizeChange();
    m_sampleOverlay->UpdateForWindowSizeChange();

    D2D1_SIZE_F size = m_d2dContext->GetSize();

    // Exclude sample title from imageable area for image drawing.
    float titleHeight = m_sampleOverlay->GetTitleHeightInDips();

    D2D1_RECT_F contentBox = D2D1::RectF(0, titleHeight, size.width, size.height);
    m_displayPageRendererContext->UpdateTargetBox(
        contentBox
        );
}

// On-screen rendering.
void PageRenderer::Render()
{
    m_d2dContext->BeginDraw();

    if (m_isSnapped)
    {
        m_displayPageRendererContext->DrawMessage("This sample does not support snapped view.");
    }
    else
    {
        // Render page context (i.e. images).
        m_displayPageRendererContext->Draw();
    }

    // We ignore D2DERR_RECREATE_TARGET here. This error indicates that the device
    // is lost. It will be handled during the next call to Present.
    HRESULT hr = m_d2dContext->EndDraw();
    if (hr != D2DERR_RECREATE_TARGET)
    {
        DX::ThrowIfFailed(hr);
    }

    // Render sample title.
    m_sampleOverlay->Render();

    // We are accessing D3D resources directly in Present() without D2D's knowledge,
    // so we must manually acquire the D2D factory lock.
    //
    // Note: it's absolutely critical that the factory lock be released upon
    // exiting this function, or else the entire app will deadlock. This is
    // ensured via the following RAII class.
    {
        D2DFactoryLock factoryLock(m_d2dFactory.Get());
        Present();
    }
}

void PageRenderer::SetSnappedMode(_In_ bool isSnapped)
{
    m_isSnapped = isSnapped;
}

void PageRenderer::DrawPreviewSurface(
    _In_  float                             width,
    _In_  float                             height,
    _In_  float                             scale,
    _In_  D2D1_RECT_F                       contentBox,
    _In_  uint32                            desiredJobPage,
    _In_  IPrintPreviewDxgiPackageTarget*   previewTarget
    )
{
    // We are accessing D3D resources directly without D2D's knowledge, so we
    // must manually acquire the D2D factory lock.
    //
    // Note: it's absolutely critical that the factory lock be released upon
    // exiting this function, or else the entire app will deadlock. This is
    // ensured via the following RAII class.
    D2DFactoryLock factoryLock(m_d2dFactory.Get());

    CD3D11_TEXTURE2D_DESC textureDesc(
        DXGI_FORMAT_B8G8R8A8_UNORM,
        static_cast<uint32>(ceil(width  * m_dpi / 96)),
        static_cast<uint32>(ceil(height * m_dpi / 96)),
        1,
        1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
        );

    ComPtr<ID3D11Texture2D> texture;
    DX::ThrowIfFailed(
        m_d3dDevice->CreateTexture2D(
            &textureDesc,
            nullptr,
            &texture
            )
        );

    // Create a preview DXGI surface with given size.
    ComPtr<IDXGISurface> dxgiSurface;
    DX::ThrowIfFailed(
        texture.As<IDXGISurface>(&dxgiSurface)
        );

    // Create a new D2D device context for rendering the preview surface.
    // D2D device contexts are stateful, and hence a unique device must be
    // used on each thread.
    ComPtr<ID2D1DeviceContext> d2dContext;
    DX::ThrowIfFailed(
        m_d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &d2dContext
            )
        );

    // Update DPI for preview surface as well.
    d2dContext->SetDpi(m_dpi, m_dpi);

    // Recommend using the screen DPI for better fidelity and better performance in the print preview
    D2D1_BITMAP_PROPERTIES1 bitmapProperties =
        D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );

    // Create surface bitmap on which page content is drawn.
    ComPtr<ID2D1Bitmap1> d2dSurfaceBitmap;
    DX::ThrowIfFailed(
        d2dContext->CreateBitmapFromDxgiSurface(
            dxgiSurface.Get(),
            &bitmapProperties,
            &d2dSurfaceBitmap
            )
        );

    d2dContext->SetTarget(d2dSurfaceBitmap.Get());

    // Create and initialize the page renderer context for preview.
    PageRendererContext^ previewPageRendererContext =
        ref new PageRendererContext(
            contentBox,
            d2dContext.Get(),
            DrawTypes::Preview,
            this
            );

    d2dContext->BeginDraw();

    // Draw page content on the preview surface.
    // Here contentBox is smaller than the real page size and the scale
    // indicates the proportion. It is faster to draw content on a smaller
    // surface. Set a global transform of 1/scale, allowing Draw to draw to a
    // virtual target box without accounting for the scale factor.
    d2dContext->SetTransform(D2D1::Matrix3x2F(1.0f / scale, 0.0f, 0.0f, 1.0f / scale, 0.0f, 0.0f));

    previewPageRendererContext->Draw();

    // The document source handles D2DERR_RECREATETARGET, so it is okay to throw this error
    // here.
    DX::ThrowIfFailed(
        d2dContext->EndDraw()
        );

    // Must pass the same DPI as used to create the DXGI surface for the correct print preview.
    DX::ThrowIfFailed(
        previewTarget->DrawPage(
            desiredJobPage,
            dxgiSurface.Get(),
            m_dpi,
            m_dpi
            )
        );
}

void PageRenderer::CreatePrintControl(
    _In_  IPrintDocumentPackageTarget*      docPackageTarget,
    _In_  D2D1_PRINT_CONTROL_PROPERTIES*    printControlProperties
    )
{
    // Explicitly release existing D2D print control.
    m_d2dPrintControl = nullptr;

    DX::ThrowIfFailed(
        m_d2dDevice->CreatePrintControl(
            m_wicFactory.Get(),
            docPackageTarget,
            printControlProperties,
            &m_d2dPrintControl
            )
        );
}

HRESULT PageRenderer::ClosePrintControl()
{
    return (m_d2dPrintControl == nullptr) ? S_OK : m_d2dPrintControl->Close();
}

// Print out one page, with the given print ticket.
// This sample has only one page and we ignore pageNumber below.
void PageRenderer::PrintPage(
    _In_ uint32                 /*pageNumber*/,
    _In_ D2D1_RECT_F            imageableArea,
    _In_ D2D1_SIZE_F            pageSize,
    _In_opt_ IStream*           pagePrintTicketStream
    )
{
    // Create a new D2D device context for generating the print command list.
    // D2D device contexts are stateful, and hence a unique device context must
    // be used on each thread.
    ComPtr<ID2D1DeviceContext> d2dContext;
    DX::ThrowIfFailed(
        m_d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &d2dContext
            )
        );

    ComPtr<ID2D1CommandList> printCommandList;
    DX::ThrowIfFailed(
        d2dContext->CreateCommandList(&printCommandList)
        );

    d2dContext->SetTarget(printCommandList.Get());

    // Create and initialize the page renderer context for print.
    // In this case, we want to use the bitmap source that already has
    // the color context embedded in it. Thus, we pass NULL for the
    // color context parameter.
    PageRendererContext^ printPageRendererContext =
        ref new PageRendererContext(
            imageableArea,
            d2dContext.Get(),
            DrawTypes::Printing,
            this
            );

    d2dContext->BeginDraw();

    // Draw page content on a command list.
    printPageRendererContext->Draw();

    // The document source handles D2DERR_RECREATETARGET, so it is okay to throw this error
    // here.
    DX::ThrowIfFailed(
        d2dContext->EndDraw()
        );

    DX::ThrowIfFailed(
        printCommandList->Close()
        );

    DX::ThrowIfFailed(
        m_d2dPrintControl->AddPage(printCommandList.Get(), pageSize, pagePrintTicketStream)
        );
}

IWICColorContext* PageRenderer::GetWicColorContextNoRef()
{
    return m_wicColorContext.Get();
}

IWICBitmapSource* PageRenderer::GetOriginalWicBitmapSourceNoRef()
{
    return m_wicConvertedSource.Get();
}

IWICBitmapSource* PageRenderer::GetWicBitmapSourceWithEmbeddedColorContextNoRef()
{
    return m_profiledWicBitmap.Get();
}

IDWriteTextFormat* PageRenderer::GetTextFormatNoRef()
{
    return m_textFormat.Get();
}

IDWriteTextFormat* PageRenderer::GetMessageFormatNoRef()
{
    return m_messageFormat.Get();
}
