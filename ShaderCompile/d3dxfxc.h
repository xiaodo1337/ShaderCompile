//====== Copyright © 1996-2006, Valve Corporation, All rights reserved. =======//
//
// Purpose: D3DX command implementation.
//
// $NoKeywords: $
//
//=============================================================================//

#pragma once

#include "basetypes.h"
#include "cmdsink.h"

#include "robin_hood.h"
#include <string>
#include <vector>

class CSharedFile final : private std::vector<char>
{
public:
	CSharedFile( std::vector<char>&& data ) noexcept;
	using std::vector<char>::vector;
	~CSharedFile() = default;

	[[nodiscard]] const void* Data() const noexcept { return data(); }
	[[nodiscard]] size_t Size() const noexcept { return size(); }
	[[nodiscard]] bool CacheSafe() const noexcept { return m_bCacheSafe; }

private:
	bool m_bCacheSafe = true;
};

class FileCache final
{
public:
	FileCache() = default;
	~FileCache() { Clear(); }

	void Add( const std::string& fileName, std::vector<char>&& data );

	[[nodiscard]] const CSharedFile* Get( const std::string& filename ) const;

	void Clear();

protected:
	typedef robin_hood::unordered_node_map<std::string, CSharedFile> Mapping;
	Mapping m_map;
};

extern FileCache fileCache;

namespace CfgProcessor
{
	struct ComboBuildCommand;
}

namespace Compiler
{
	// Cache configuration and lifecycle are controlled between shader ranges.
	void SetPreprocessCacheEnabled( bool enabled );
	void BeginPreprocessCacheRange();
	void EndPreprocessCacheRange();
	void ClearCompileCache();
	void ExecuteCommand( const CfgProcessor::ComboBuildCommand& pCommand, CmdSink::IResponse* &ppResponse, unsigned int flags );
}; // namespace InterceptFxc