#include <cstdio>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <gzip/config.hpp>
#include <gzip/decompress.hpp>
#include <gzip/utils.hpp>
#include <gzip/version.hpp>
#include "mongoose/mongoose.h"

using namespace std;

static const char* s_http_port = "12345";
static const string SavePath = "/home/crashreports/";

static struct mg_serve_http_opts s_http_server_opts;

static unsigned int ReadUint32(const char* Data, const int Pos)
{
	unsigned int result = 0;
	result = *(const unsigned char*)(Data + Pos + 3);
	result = *(const unsigned char*)(Data + Pos + 2) + (result << 8);
	result = *(const unsigned char*)(Data + Pos + 1) + (result << 8);
	result = *(const unsigned char*)(Data + Pos + 0) + (result << 8);
	return result;
}

static void handle_crashreport(struct mg_connection* nc, int ev, void* p)
{
	switch (ev)
	{
	case MG_EV_HTTP_REQUEST:
	{
		const struct http_message* hm = (struct http_message*) p;

		// block everyone but the crash report client
		const mg_str* userAgent = mg_get_http_header(hm, "User-Agent");
		const char* prefix = "CrashReportClient";
		if (strncmp(prefix, userAgent->p, strlen(prefix)) != 0)
		{
			nc->flags |= MG_F_CLOSE_IMMEDIATELY;
			return;
		}

		const mg_str* numberOfFiles = mg_get_http_header(hm, "NumberOfFiles");
		if (!numberOfFiles)
		{
			nc->flags |= MG_F_CLOSE_IMMEDIATELY;
			return;
		}

		// optional
		const mg_str* steamName = mg_get_http_header(hm, "SteamName");
		string sSteamName = "";
		if (steamName)
			sSteamName = string(steamName->p, steamName->len);

		try
		{
			const string sNumberOfFiles = numberOfFiles->p;
			const unsigned int nFiles = stoul(sNumberOfFiles);

			if (nFiles <= 0)
			{
				nc->flags |= MG_F_CLOSE_IMMEDIATELY;
				return;
			}

			const string decompressed = gzip::decompress(hm->body.p, hm->body.len);

			// parse the decompressed blob and save the real files to disk
			// using c_str() with substr() since UE4 saves at least 260 characters for a string, regardless of its actual size
			if (decompressed.size() > 0)
			{
				const char* data = decompressed.data();
				unsigned int filepos = 0;

				const unsigned int dirnameSize = ReadUint32(data, filepos);
				filepos += 4;

				const string dirname = decompressed.substr(filepos, dirnameSize).c_str();
				filepos += dirnameSize;

				const unsigned int filenameSize = ReadUint32(data, filepos);
				filepos += 4;

				const string filename = decompressed.substr(filepos, filenameSize).c_str();
				filepos += filenameSize;

				// ignored
				const unsigned int decompressedBlobSize = ReadUint32(data, filepos);
				filepos += 4;

				// ignored, using the one from the HTTP header
				const unsigned int fileCount = ReadUint32(data, filepos);
				filepos += 4;

				// for the mkdir flags, see https://techoverflow.net/2013/04/05/how-to-use-mkdir-from-sysstat-h/
				const string dirPath = SavePath + (sSteamName.size() > 0 ? sSteamName + "__" : "") + dirname + "/";
				mkdir(dirPath.c_str(), S_IRWXU); // 0700

				for (unsigned int i = 0; i < nFiles; i++)
				{
					const unsigned int curFileIndex = ReadUint32(data, filepos);
					filepos += 4;

					const unsigned int curFilenameSize = ReadUint32(data, filepos);
					filepos += 4;

					const string curFilename = decompressed.substr(filepos, curFilenameSize).c_str();
					filepos += curFilenameSize;

					const unsigned int curFiledataSize = ReadUint32(data, filepos);
					filepos += 4;

					const string curFiledata = decompressed.substr(filepos, curFiledataSize);
					filepos += curFiledataSize;

					const string curFilepath = dirPath + curFilename;
					FILE* curFile = fopen(curFilepath.c_str(), "wb");

					if (curFile == NULL)
					{
						nc->flags |= MG_F_CLOSE_IMMEDIATELY;
						return;
					}

					fwrite(curFiledata.data(), 1, curFiledata.size(), curFile);
					fclose(curFile);
				}
			}
			else
			{
			    // this is how to send an error code back, if I ever want to
			    //mg_printf(nc, "%s", "HTTP/1.1 500\r\nContent-Length: 0\r\n\r\n");
				//nc->flags |= MG_F_SEND_AND_CLOSE;

				nc->flags |= MG_F_CLOSE_IMMEDIATELY;
				return;
			}

			mg_printf(nc, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
			nc->flags |= MG_F_SEND_AND_CLOSE;
		}
		catch (exception& ex)
		{
			nc->flags |= MG_F_CLOSE_IMMEDIATELY;
			return;
		}

		break;
	}
	case MG_EV_HTTP_REPLY:
	{
		break;
	}
	default:
	{
		nc->flags |= MG_F_CLOSE_IMMEDIATELY;
		break;
	}
	}
}

static void ev_handler(struct mg_connection* nc, int ev, void* ev_data)
{
	if (ev == MG_EV_HTTP_REQUEST)
		mg_serve_http(nc, (http_message*)ev_data, s_http_server_opts);
}

int main(int argc, char** argv)
{
	struct mg_mgr mgr;
	struct mg_connection* c;

	mg_mgr_init(&mgr, NULL);
	c = mg_bind(&mgr, s_http_port, ev_handler);
	if (c == NULL)
	{
		fprintf(stderr, "Cannot start server on port %s\n", s_http_port);
		exit(EXIT_FAILURE);
	}

	s_http_server_opts.document_root = SavePath.c_str();

	mg_register_http_endpoint(c, "/crashreport", handle_crashreport MG_UD_ARG(NULL));
	mg_set_protocol_http_websocket(c);
	printf("Starting crash report web server on port %s\n", s_http_port);

	for (;;)
		mg_mgr_poll(&mgr, 1000); // one second

	printf("Stopping crash report web server\n");
	mg_mgr_free(&mgr);
	return EXIT_SUCCESS;
}
