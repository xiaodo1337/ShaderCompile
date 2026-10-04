#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "gsl/narrow"
#include "CRC32.hpp"

// One append-only file per shader/configuration, rather than a file per combo.
// Only complete, successful compressed static combos may be committed here.
class ResumeJournal
{
public:
	template <typename Restore>
	void Open( const std::filesystem::path& path, const std::string& identity,
		uint64_t staticCount, bool force, Restore restore )
	{
		std::filesystem::create_directories( path.parent_path() );
		std::ifstream input( path, std::ios::binary );
		uint64_t magic = 0;
		uint32_t identitySize = 0;
		std::string savedIdentity;
		bool valid = !force && Read( input, magic ) && magic == Magic &&
			Read( input, identitySize ) && identitySize == identity.size();
		if ( valid )
		{
			savedIdentity.resize( identitySize );
			valid = bool( input.read( savedIdentity.data(), identitySize ) ) && savedIdentity == identity;
		}
		if ( !valid )
		{
			input.close();
			std::ofstream output( path, std::ios::binary | std::ios::trunc );
			Write( output, Magic );
			const auto size = static_cast<uint32_t>( identity.size() );
			Write( output, size );
			output.write( identity.data(), size );
			output.flush();
			if ( !output )
				throw std::runtime_error( "Cannot create shader resume journal" );
		}
		else
		{
			uintmax_t end = static_cast<uintmax_t>( input.tellg() );
			for ( ;; )
			{
				uint64_t id = 0, size = 0;
				uint32_t crc = 0;
				if ( !Read( input, id ) || !Read( input, size ) || !Read( input, crc ) ||
					id >= staticCount || !size || size > MaxBlockSize )
					break;
				std::vector<uint8_t> block( static_cast<size_t>( size ) );
				if ( !input.read( reinterpret_cast<char*>( block.data() ), static_cast<std::streamsize>( size ) ) ||
					Checksum( id, size, block.data() ) != crc )
					break;
				restore( id, block );
				end = static_cast<uintmax_t>( input.tellg() );
			}
			input.close();
			// A killed process may leave a partial record. Preserve the valid prefix.
			std::filesystem::resize_file( path, end );
		}
		m_Output.open( path, std::ios::binary | std::ios::app );
		if ( !m_Output )
			throw std::runtime_error( "Cannot append shader resume journal" );
	}

	void Append( uint64_t id, const void* data, size_t size )
	{
		if ( !size || size > MaxBlockSize )
			throw std::runtime_error( "Invalid shader resume block size" );
		const auto length = static_cast<uint64_t>( size );
		const uint32_t crc = Checksum( id, length, data );
		std::lock_guard guard{ m_Mutex };
		Write( m_Output, id );
		Write( m_Output, length );
		Write( m_Output, crc );
		m_Output.write( static_cast<const char*>( data ), static_cast<std::streamsize>( size ) );
		m_Output.flush();
		if ( !m_Output )
			throw std::runtime_error( "Shader resume journal write failed (check free disk space)" );
	}

private:
	static constexpr uint64_t Magic = 0x31524a4353435348ULL;
	static constexpr uint64_t MaxBlockSize = 256ULL * 1024 * 1024;
	std::ofstream m_Output;
	std::mutex m_Mutex;

	template <typename T> static bool Read( std::istream& input, T& value )
	{
		return bool( input.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) );
	}
	template <typename T> static void Write( std::ostream& output, const T& value )
	{
		output.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
	}
	static uint32_t Checksum( uint64_t id, uint64_t size, const void* data )
	{
		CRC32::CRC32_t crc;
		CRC32::Init( crc );
		// The legacy ProcessBuffer uses unaligned uint32 loads for short tails.
		const auto bytes = [&]( const void* source, size_t length )
		{
			const auto* input = static_cast<const uint8_t*>( source );
			for ( size_t i = 0; i < length; ++i )
				crc = CRC32::pulCRCTable[( crc ^ input[i] ) & 255] ^ ( crc >> 8 );
		};
		bytes( &id, sizeof( id ) );
		bytes( &size, sizeof( size ) );
		bytes( data, static_cast<size_t>( size ) );
		CRC32::Final( crc );
		return crc;
	}
};
