//====== Copyright © 1996-2006, Valve Corporation, All rights reserved. =======//
//
// Purpose: D3DX command implementation.
//
// $NoKeywords: $
//
//=============================================================================//

#define WIN32_LEAN_AND_MEAN
#define NOWINRES
#define NOSERVICE
#define NOMCX
#define NOIME
#define NOMINMAX

#include "d3dxfxc.h"

#include "basetypes.h"
#include "cfgprocessor.h"
#include "cmdsink.h"
#include "d3dcompiler.h"
#include "gsl/narrow"
#include <malloc.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <optional>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <filesystem>
#pragma comment( lib, "D3DCompiler" )

namespace fs = std::filesystem;

CSharedFile::CSharedFile( std::vector<char>&& data ) noexcept : std::vector<char>( std::forward<std::vector<char>>( data ) )
{
	const std::string_view source( this->data(), size() );
	m_bCacheSafe = true;
	for ( const char* macro : { "__DATE__", "__TIME__", "__COUNTER__", "__SHADER_TARGET_", "__HLSL_VERSION" } )
		m_bCacheSafe = m_bCacheSafe && source.find( macro ) == std::string_view::npos;
}

void FileCache::Add( const std::string& fileName, std::vector<char>&& data )
{
	fs::path path = fileName;
	std::string baseFileName = path.filename().string();

	const auto& it = m_map.find( baseFileName );
	if ( it != m_map.end() )
		return;

	CSharedFile file( std::forward<std::vector<char>>( data ) );
	m_map.emplace( baseFileName, std::move( file ) );
}

const CSharedFile* FileCache::Get( const std::string& filename ) const
{
	// Search the cache first
	const auto find = m_map.find( filename );
	if ( find != m_map.cend() )
		return &find->second;
	return nullptr;
}

void FileCache::Clear()
{
	m_map.clear();
}

FileCache fileCache;

struct DxIncludeImpl final : public ID3DInclude
{
	std::atomic<bool> cacheSafe{ true };

	STDMETHOD( Open )( THIS_ D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID, LPCVOID* ppData, UINT* pBytes ) override
	{
		const CSharedFile* file = fileCache.Get( pFileName );
		if ( !file )
			return E_FAIL;

		if ( !file->CacheSafe() )
			cacheSafe.store( false );
		*ppData = file->Data();
		*pBytes = gsl::narrow<UINT>( file->Size() );

		return S_OK;
	}

	STDMETHOD( Close )( THIS_ LPCVOID ) override
	{
		return S_OK;
	}

	virtual ~DxIncludeImpl() = default;
};

struct CompileResult
{
	ID3DBlob* shader = nullptr;
	ID3DBlob* listing = nullptr;
	HRESULT status = E_FAIL;

	~CompileResult()
	{
		if ( shader )
			shader->Release();
		if ( listing )
			listing->Release();
	}
	CompileResult() = default;
	CompileResult( const CompileResult& ) = delete;
	CompileResult& operator=( const CompileResult& ) = delete;
};

class CResponse final : public CmdSink::IResponse
{
public:
	explicit CResponse( std::shared_ptr<const CompileResult> result ) noexcept : m_Result( std::move( result ) ) {}
	bool Succeeded() const noexcept override { return m_Result->shader && m_Result->status == S_OK; }
	size_t GetResultBufferLen() const override { return Succeeded() ? m_Result->shader->GetBufferSize() : 0; }
	const void* GetResultBuffer() const override { return Succeeded() ? m_Result->shader->GetBufferPointer() : nullptr; }
	const char* GetListing() const override { return static_cast<const char*>( m_Result->listing ? m_Result->listing->GetBufferPointer() : nullptr ); }

private:
	std::shared_ptr<const CompileResult> m_Result;
};

namespace
{
	using ResultFuture = std::shared_future<std::shared_ptr<const CompileResult>>;
	struct CacheKey
	{
		std::string text;
		size_t hash;
		explicit CacheKey( std::string value ) : text( std::move( value ) ), hash( std::hash<std::string>{}( text ) ) {}
		bool operator==( const CacheKey& other ) const noexcept { return text == other.text; }
	};
	struct CacheKeyHash
	{
		size_t operator()( const CacheKey& key ) const noexcept { return key.hash; }
	};
	std::mutex s_CacheMutex;
	std::unordered_map<CacheKey, ResultFuture, CacheKeyHash> s_CompileCache;
	size_t s_CacheBytes = 0;
	uint64_t s_CacheGeneration = 0;
	bool s_bPreprocessCache = true; // Configuration, set before workers start.
	std::atomic<bool> s_bAdaptivePreprocessCache{ true };
	std::atomic<uint32_t> s_PreprocessSamples{ 0 };
	std::atomic<uint32_t> s_PreprocessHits{ 0 };
	constexpr uint32_t AdaptiveSampleCount = 256;
	constexpr uint32_t AdaptiveMinimumHitPercent = 1;
	constexpr size_t MaxCacheBytes = 64 * 1024 * 1024;
	constexpr size_t MaxCacheEntries = 4096;

	void ClearCacheUnlocked()
	{
		s_CompileCache.clear();
		s_CacheBytes = 0;
		++s_CacheGeneration;
	}

	std::shared_ptr<const CompileResult> Compile( const CfgProcessor::ComboBuildCommand& command,
		const CSharedFile& source, const D3D_SHADER_MACRO* macros, unsigned int flags )
	{
		auto result = std::make_shared<CompileResult>();
		DxIncludeImpl includes;
		result->status = D3DCompile( source.Data(), source.Size(), command.fileName.data(), macros, &includes,
			command.entryPoint.data(), command.shaderModel.data(), flags, 0, &result->shader, &result->listing );
		return result;
	}

	void AppendKeyField( std::string& key, std::string_view value )
	{
		const uint64_t size = value.size();
		key.append( reinterpret_cast<const char*>( &size ), sizeof( size ) );
		key.append( value.data(), value.size() );
	}
}

void Compiler::SetPreprocessCacheEnabled( bool enabled )
{
	s_bPreprocessCache = enabled;
	s_bAdaptivePreprocessCache.store( enabled, std::memory_order_release );
	ClearCompileCache();
}

void Compiler::BeginPreprocessCacheRange()
{
	s_PreprocessSamples.store( 0, std::memory_order_relaxed );
	s_PreprocessHits.store( 0, std::memory_order_relaxed );
	s_bAdaptivePreprocessCache.store( s_bPreprocessCache, std::memory_order_release );
	ClearCompileCache();
}

void Compiler::EndPreprocessCacheRange()
{
	s_bAdaptivePreprocessCache.store( false, std::memory_order_release );
	ClearCompileCache();
}

void Compiler::ClearCompileCache()
{
	std::lock_guard lock{ s_CacheMutex };
	ClearCacheUnlocked();
}

void Compiler::ExecuteCommand( const CfgProcessor::ComboBuildCommand& command, CmdSink::IResponse*& response, unsigned int flags )
{
	static thread_local std::vector<D3D_SHADER_MACRO> macros;
	macros.resize( command.defines.size() + 1 );
	std::transform( command.defines.begin(), command.defines.end(), macros.begin(),
		[]( const auto& define ) { return D3D_SHADER_MACRO{ define.first.data(), define.second.data() }; } );
	macros.back() = { nullptr, nullptr };

	const CSharedFile* source = fileCache.Get( std::string( command.fileName ) );
	if ( !source )
	{
		response = new CResponse( std::make_shared<CompileResult>() );
		return;
	}
	// Debug output must retain the original source and macro information.
	if ( !s_bAdaptivePreprocessCache.load( std::memory_order_acquire ) || ( flags & D3DCOMPILE_DEBUG ) || !source->CacheSafe() )
	{
		response = new CResponse( Compile( command, *source, macros.data(), flags ) );
		return;
	}

	CompileResult preprocessed;
	DxIncludeImpl includes;
	preprocessed.status = D3DPreprocess( source->Data(), source->Size(), command.fileName.data(), macros.data(),
		&includes, &preprocessed.shader, &preprocessed.listing );
	// Preserve the original compiler diagnostics for preprocessing errors or warnings.
	if ( FAILED( preprocessed.status ) || !preprocessed.shader || preprocessed.listing || !includes.cacheSafe.load() )
	{
		response = new CResponse( Compile( command, *source, macros.data(), flags ) );
		return;
	}

	std::string key;
	key.reserve( preprocessed.shader->GetBufferSize() + 128 );
	AppendKeyField( key, command.fileName );
	AppendKeyField( key, command.entryPoint );
	AppendKeyField( key, command.shaderModel );
	key.append( reinterpret_cast<const char*>( &flags ), sizeof( flags ) );
	key.append( static_cast<const char*>( preprocessed.shader->GetBufferPointer() ), preprocessed.shader->GetBufferSize() );
	if ( key.size() > MaxCacheBytes )
	{
		response = new CResponse( Compile( command, *source, macros.data(), flags ) );
		return;
	}

	preprocessed.shader->Release();
	preprocessed.shader = nullptr;
	CacheKey cacheKey( std::move( key ) ); // Hash outside the cache lock.
	ResultFuture future;
	std::optional<std::promise<std::shared_ptr<const CompileResult>>> promise;
	bool producer = false;
	uint64_t generation = 0;
	{
		std::lock_guard lock{ s_CacheMutex };
		const auto found = s_CompileCache.find( cacheKey );
		s_PreprocessSamples.fetch_add( 1, std::memory_order_relaxed );
		if ( found != s_CompileCache.end() )
		{
			s_PreprocessHits.fetch_add( 1, std::memory_order_relaxed );
			future = found->second;
		}
		else
		{
			if ( s_CompileCache.size() >= MaxCacheEntries || s_CacheBytes + cacheKey.text.size() > MaxCacheBytes )
				ClearCacheUnlocked();
			promise.emplace();
			future = promise->get_future().share();
			s_CacheBytes += cacheKey.text.size();
			s_CompileCache.emplace( std::move( cacheKey ), future );
			generation = s_CacheGeneration;
			producer = true;
		}
	}
	const uint32_t samples = s_PreprocessSamples.load( std::memory_order_relaxed );
	if ( samples >= AdaptiveSampleCount && s_bAdaptivePreprocessCache.load( std::memory_order_acquire ) )
	{
		const uint32_t hits = s_PreprocessHits.load( std::memory_order_relaxed );
		if ( hits * 100 < samples * AdaptiveMinimumHitPercent )
		{
			bool expected = true;
			if ( s_bAdaptivePreprocessCache.compare_exchange_strong( expected, false, std::memory_order_acq_rel ) )
				ClearCompileCache();
		}
	}
	if ( producer )
	{
		try
		{
			// Cache misses still compile the original input; only equal expanded inputs reuse results.
			auto result = Compile( command, *source, macros.data(), flags );
			{
				std::lock_guard lock{ s_CacheMutex };
				if ( generation == s_CacheGeneration )
				{
					s_CacheBytes += ( result->shader ? result->shader->GetBufferSize() : 0 ) +
						( result->listing ? result->listing->GetBufferSize() : 0 );
					if ( s_CacheBytes > MaxCacheBytes )
						ClearCacheUnlocked();
				}
			}
			promise->set_value( std::move( result ) );
		}
		catch ( ... )
		{
			promise->set_exception( std::current_exception() );
			throw;
		}
	}
	response = new CResponse( future.get() );
}
