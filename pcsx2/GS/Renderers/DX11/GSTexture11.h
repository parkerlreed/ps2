/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "GS/GS.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "common/RedtapeWindows.h"
#include "common/RedtapeWilCom.h"
#include <d3d11.h>
#include <memory>

class GSTexture11 final : public GSTexture
{
	wil::com_ptr_nothrow<ID3D11Texture2D> m_texture;
	wil::com_ptr_nothrow<ID3D11ShaderResourceView> m_srv;
	wil::com_ptr_nothrow<ID3D11RenderTargetView> m_rtv;
	wil::com_ptr_nothrow<ID3D11DepthStencilView> m_dsv;
	wil::com_ptr_nothrow<ID3D11UnorderedAccessView> m_uav;
	D3D11_TEXTURE2D_DESC m_desc;

public:
	explicit GSTexture11(wil::com_ptr_nothrow<ID3D11Texture2D> texture, const D3D11_TEXTURE2D_DESC& desc,
		GSTexture::Type type, GSTexture::Format format);

	static DXGI_FORMAT GetDXGIFormat(Format format);

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = NULL, int layer = 0) override;
	void Unmap() override;
	void GenerateMipmap() override;

	operator ID3D11Texture2D*();
	operator ID3D11ShaderResourceView*();
	operator ID3D11RenderTargetView*();
	operator ID3D11DepthStencilView*();
	operator ID3D11UnorderedAccessView*();
};

class GSDownloadTexture11 final : public GSDownloadTexture
{
public:
	~GSDownloadTexture11() override;

	static std::unique_ptr<GSDownloadTexture11> Create(u32 width, u32 height, GSTexture::Format format);

	void CopyFromTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch) override;

	bool Map(const GSVector4i& rc) override;
	void Unmap() override;

	void Flush() override;

private:
	GSDownloadTexture11(wil::com_ptr_nothrow<ID3D11Texture2D> tex, u32 width, u32 height, GSTexture::Format format);

	wil::com_ptr_nothrow<ID3D11Texture2D> m_texture;
};
