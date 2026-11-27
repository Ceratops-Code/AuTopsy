/**********************************************************************

  Audacity: A Digital Audio Editor

  FFmpegLogImpl.inl

  Dmitry Vedenko

**********************************************************************/

//! It is assumed that the lifetime of this is between loading and unloading
//! of ffmpeg libraries.
class FFmpegLogImpl : public FFmpegLog
{
public:
   using Callback = void (*)(void*, int, const char*, va_list);
   using CallbackSetter = void (*)(Callback);
   explicit FFmpegLogImpl(
      CallbackSetter av_log_set_callback, Callback oldCallback)
      : mCallbackSetter{ av_log_set_callback }
      , mOldCallback{ oldCallback }
   {
      if (mCallbackSetter)
         mCallbackSetter(LogCallback);
   }
   ~FFmpegLogImpl() override
   {
      if (mCallbackSetter)
         mCallbackSetter(mOldCallback);
   }

private:
   static void LogCallback(void* ptr, int level, const char* fmt, va_list vl)
   {
      if (level > AV_LOG_INFO)
         return;

      wxString message;

      if (ptr != nullptr)
      {
         AVClass* cls = *static_cast<AVClass**>(ptr);

         message = wxString::Format(
            wxT("[%s @ %p] "),
            wxString::FromUTF8(cls->item_name(ptr)),
            ptr
         );
      }

      // FFmpeg's variadic arguments use the narrow C printf conventions.
      char buffer[1024];
      va_list args;
      va_copy(args, vl);
      const int length = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
      va_end(args);
      if (length < 0)
         return;

      if (static_cast<size_t>(length) < sizeof(buffer))
         message += wxString::FromUTF8(buffer, length);
      else
      {
         std::string fullMessage(static_cast<size_t>(length) + 1, '\0');
         va_copy(args, vl);
         const int result = std::vsnprintf(
            fullMessage.data(), fullMessage.size(), fmt, args);
         va_end(args);
         if (result != length)
            return;
         message += wxString::FromUTF8(fullMessage.data(), length);
      }

      wxString cpt;
      switch (level)
      {
      case 0:
         cpt = wxT("Error");
         wxLogError(wxT("%s"), message);
         break;
      case 1:
         cpt = wxT("Info");
         wxLogInfo(wxT("%s"), message);
         break;
      case 2:
         cpt = wxT("Debug");
         wxLogInfo(wxT("%s"), message);
         break;
      default:
         cpt = wxT("Log");
         wxLogInfo(wxT("%s"), message);
         break;
      }

      wxLogDebug(wxT("%s: %s"), cpt, message);
   }

   CallbackSetter mCallbackSetter;
   Callback mOldCallback;
};

std::unique_ptr<FFmpegLog> CreateLogCallbackSetter(const FFmpegFunctions& ffmpeg)
{

   return std::make_unique<FFmpegLogImpl>(
      ffmpeg.av_log_set_callback, ffmpeg.av_log_default_callback);
}
