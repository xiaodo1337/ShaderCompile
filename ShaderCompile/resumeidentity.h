#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <d3dcompiler.h>
#include <array>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <limits>
#include <vector>

#pragma comment( lib, "bcrypt" )

class ResumeIdentity
{
public:
	ResumeIdentity()
	{
		if ( BCryptOpenAlgorithmProvider( &m_Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) < 0 )
			throw std::runtime_error( "Cannot initialize SHA256 provider" );
		ULONG size = 0, written = 0;
		if ( BCryptGetProperty( m_Algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>( &size ), sizeof( size ), &written, 0 ) < 0 )
		{
			BCryptCloseAlgorithmProvider( m_Algorithm, 0 );
			throw std::runtime_error( "Cannot query SHA256 provider" );
		}
		m_Object.resize( size );
		if ( BCryptCreateHash( m_Algorithm, &m_Hash, m_Object.data(), size, nullptr, 0, 0 ) < 0 )
		{
			BCryptCloseAlgorithmProvider( m_Algorithm, 0 );
			throw std::runtime_error( "Cannot initialize SHA256 hash" );
		}
	}
	~ResumeIdentity()
	{
		BCryptDestroyHash( m_Hash );
		BCryptCloseAlgorithmProvider( m_Algorithm, 0 );
	}
	ResumeIdentity( const ResumeIdentity& ) = delete;
	ResumeIdentity& operator=( const ResumeIdentity& ) = delete;

	void Add( std::string_view value )
	{
		const uint64_t size = value.size();
		Bytes( &size, sizeof( size ) );
		Bytes( value.data(), value.size() );
	}
	void File( const std::filesystem::path& path )
	{
		Add( path.generic_string() );
		std::ifstream input( path, std::ios::binary );
		if ( !input )
			throw std::runtime_error( "Cannot fingerprint " + path.string() );
		const uint64_t size = std::filesystem::file_size( path );
		Bytes( &size, sizeof( size ) );
		std::array<char, 65536> buffer;
		while ( input.read( buffer.data(), static_cast<std::streamsize>( buffer.size() ) ) || input.gcount() )
			Bytes( buffer.data(), static_cast<size_t>( input.gcount() ) );
		if ( input.bad() )
			throw std::runtime_error( "Cannot read fingerprint input " + path.string() );
	}
	std::string Finish()
	{
		std::array<unsigned char, 32> digest;
		if ( BCryptFinishHash( m_Hash, digest.data(), static_cast<ULONG>( digest.size() ), 0 ) < 0 )
			throw std::runtime_error( "Cannot finish SHA256 hash" );
		std::string result;
		for ( unsigned char byte : digest )
		{
			result += "0123456789abcdef"[byte >> 4];
			result += "0123456789abcdef"[byte & 15];
		}
		return result;
	}
	static std::string Toolchain()
	{
		ResumeIdentity identity;
		std::vector<wchar_t> path( 32768 );
		HMODULE compiler = nullptr;
		if ( !GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>( &D3DCompile ), &compiler ) )
			throw std::runtime_error( "Cannot locate D3D compiler module" );
		for ( HMODULE module : { static_cast<HMODULE>( nullptr ), compiler } )
		{
			DWORD size = GetModuleFileNameW( module, path.data(), static_cast<DWORD>( path.size() ) );
			if ( !size || size >= path.size() )
				throw std::runtime_error( "Cannot locate compiler binary" );
			identity.File( std::filesystem::path( std::wstring( path.data(), size ) ) );
		}
		return identity.Finish();
	}

private:
	BCRYPT_ALG_HANDLE m_Algorithm = nullptr;
	BCRYPT_HASH_HANDLE m_Hash = nullptr;
	std::vector<unsigned char> m_Object;
	void Bytes( const void* data, size_t size )
	{
		if ( !size )
			return;
		if ( size > ( std::numeric_limits<ULONG>::max )() || BCryptHashData( m_Hash,
			const_cast<PUCHAR>( static_cast<const unsigned char*>( data ) ), static_cast<ULONG>( size ), 0 ) < 0 )
			throw std::runtime_error( "Cannot hash shader configuration" );
	}
};

// A separate OS-held lock prevents two processes appending the same journal.
class ResumeLock
{
public:
	explicit ResumeLock( const std::filesystem::path& path )
	{
		m_Handle = CreateFileW( path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( m_Handle == INVALID_HANDLE_VALUE )
			throw std::runtime_error( "Cannot lock shader cache: another compiler may be using this shader" );
	}
	~ResumeLock() { CloseHandle( m_Handle ); }
	ResumeLock( const ResumeLock& ) = delete;
	ResumeLock& operator=( const ResumeLock& ) = delete;
private:
	HANDLE m_Handle = INVALID_HANDLE_VALUE;
};
