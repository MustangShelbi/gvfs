#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gfiledaemon.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gfileinputstreamdaemon.h>
#include <glib/gi18n-lib.h>

static void g_file_daemon_file_iface_init (GFileIface       *iface);

struct _GFileDaemon
{
  GObject parent_instance;

  const GDaemonMountInfo *info;
  char *path;
};

G_DEFINE_TYPE_WITH_CODE (GFileDaemon, g_file_daemon, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_file_daemon_file_iface_init))

static void
g_file_daemon_finalize (GObject *object)
{
  GFileDaemon *daemon_file;

  daemon_file = G_FILE_DAEMON (object);

  g_free (daemon_file->path);
  
  if (G_OBJECT_CLASS (g_file_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_daemon_parent_class)->finalize) (object);
}

static void
g_file_daemon_class_init (GFileDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_daemon_finalize;
}

static void
g_file_daemon_init (GFileDaemon *daemon_file)
{
}

GFile *
g_file_daemon_new (const GDaemonMountInfo *info,
		   const char *path)
{
  GFileDaemon *daemon_file;
  int len;

  daemon_file = g_object_new (G_TYPE_FILE_DAEMON, NULL);
  /* TODO: These should be construct only properties */
  daemon_file->info = info;
  daemon_file->path = g_strdup (path);

  /* Remove any trailing slashes */
  len = strlen (daemon_file->path);
  while (len > 1 && daemon_file->path[len-1] == '/')
    {
      daemon_file->path[len-1] = 0;
      len--;
    }
  
  return G_FILE (daemon_file);
}

static gboolean
g_file_daemon_is_native (GFile *file)
{
  return FALSE;
}

static char *
g_file_daemon_get_path (GFile *file)
{
  return NULL;
}

static char *
g_file_daemon_get_uri (GFile *file)
{
  /* TODO: implement to-uri */
  return NULL;
}

static char *
g_file_daemon_get_parse_name (GFile *file)
{
  /* TODO: implement to-iri */
  return NULL;
}

static GFile *
g_file_daemon_get_parent (GFile *file)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  const char *path;
  GFileDaemon *parent;
  const char *base;
  char *parent_path;
  gsize len;    

  path = daemon_file->path;
  base = strrchr (path, '/');
  if (base == NULL || base == path)
    return NULL;

  while (base > path && *base == '/')
    base--;

  len = (guint) 1 + base - path;
  
  parent_path = g_new (gchar, len + 1);
  g_memmove (parent_path, path, len);
  parent_path[len] = 0;

  parent = g_object_new (G_TYPE_FILE_DAEMON, NULL);
  parent->info = daemon_file->info;
  parent->path = parent_path;
  
  return G_FILE (parent);
}

static GFile *
g_file_daemon_copy (GFile *file)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);

  return g_file_daemon_new (daemon_file->info,
			    daemon_file->path);
}


static GFile *
g_file_daemon_get_child (GFile *file,
			 const char *name)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  char *path;
  GFile *child;

  path = g_build_filename (daemon_file->path, name, NULL);
  child = g_file_daemon_new (daemon_file->info, path);
  g_free (path);
  
  return child;
}

static GFileEnumerator *
g_file_daemon_enumerate_children (GFile      *file,
				  GFileInfoRequestFlags requested,
				  const char *attributes,
				  gboolean follow_symlinks)
{
  /* TODO: implement */
  return NULL;
}

static GFileInfo *
g_file_daemon_get_info (GFile                *file,
			GFileInfoRequestFlags requested,
			const char           *attributes,
			gboolean              follow_symlinks,
			GError              **error)
{
 
  /* TODO: implement */
  return NULL;
}

typedef struct {
  GFile *file;
  GFileReadCallback read_callback;
  gpointer callback_data;
  gboolean can_seek;
} GetFDData;

static void
read_async_get_fd_cb (int fd,
		      gpointer callback_data)
{
  GetFDData *data = callback_data;
  GFileInputStream *stream;
  GError *error;
  
  if (fd == -1)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Didn't get stream file descriptor"));
      data->read_callback (data->file, NULL, data->callback_data, error);
      g_error_free (error);
    }
  else
    {
      stream = g_file_input_stream_daemon_new (fd, data->can_seek);
      data->read_callback (data->file, stream, data->callback_data, NULL);
      g_object_unref (stream);
    }
  g_free (data);
}

static void
read_async_cb (DBusMessage *reply,
	       DBusConnection *connection,
	       GError *io_error,
	       GCancellable *cancellable,
	       gpointer op_callback,
	       gpointer op_callback_data,
	       gpointer callback_data)
{
  GFileReadCallback read_callback = op_callback;
  GFile *file = callback_data;
  GError *error;
  guint32 fd_id;
  dbus_bool_t can_seek;
  GetFDData *get_fd_data;
  
  if (io_error != NULL)
    {
      read_callback (file, NULL, op_callback_data, io_error);
    }
  else
    {
      if (!dbus_message_get_args (reply, NULL,
				  DBUS_TYPE_UINT32, &fd_id,
				  DBUS_TYPE_BOOLEAN, &can_seek,
				  DBUS_TYPE_INVALID))
	{
	  error = NULL;
	  g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       _("Invalid return value from open"));
	  read_callback (file, NULL, op_callback_data, error);
	  g_error_free (error);
	}

      get_fd_data = g_new0 (GetFDData, 1);
      get_fd_data->file = file;
      get_fd_data->read_callback = read_callback;
      get_fd_data->callback_data = op_callback_data;
      get_fd_data->can_seek = can_seek;
      
      _g_dbus_connection_get_fd_async (connection, fd_id,
				       read_async_get_fd_cb, get_fd_data);
    }
}

static void
g_file_daemon_read_async (GFile *file,
			  int io_priority,
			  GFileReadCallback callback,
			  gpointer callback_data,
			  GMainContext *context,
			  GCancellable *cancellable)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  DBusMessage *message;
  DBusMessageIter iter;
  
  message = dbus_message_new_method_call (daemon_file->info->bus_name,
					  daemon_file->info->object_path,
					  G_VFS_DBUS_MOUNTPOINT_INTERFACE,
					  G_VFS_DBUS_OP_OPEN_FOR_READ);
  
  dbus_message_iter_init_append (message, &iter);
  if (!_g_dbus_message_iter_append_filename (&iter, daemon_file->path))
    g_error ("Out of memory appending filename");

  _g_vfs_daemon_call_async (daemon_file->info->bus_name,
			    message,
			    context,
			    callback, callback_data,
			    read_async_cb, file,
			    cancellable);
  dbus_message_unref (message);
}

static GFileInputStream *
g_file_daemon_read (GFile *file,
		    GCancellable *cancellable,
		    GError **error)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  DBusConnection *connection;
  int fd;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  guint32 fd_id;
  dbus_bool_t can_seek;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }

  message = dbus_message_new_method_call (daemon_file->info->bus_name,
					  daemon_file->info->object_path,
					  G_VFS_DBUS_MOUNTPOINT_INTERFACE,
					  G_VFS_DBUS_OP_OPEN_FOR_READ);
  
  dbus_message_iter_init_append (message, &iter);
  if (!_g_dbus_message_iter_append_filename (&iter, daemon_file->path))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
		   _("Out of memory"));
      return NULL;
    }

  reply = _g_vfs_daemon_call_sync (daemon_file->info->bus_name,
				   message,
				   &connection,
				   cancellable, error);
  dbus_message_unref (message);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_file_input_stream_daemon_new (fd, can_seek);
}

static GFileOutputStream *
g_file_daemon_append_to (GFile *file,
			 GCancellable *cancellable,
			 GError **error)
{
  /* TODO: implement */
  return NULL;
}

static GFileOutputStream *
g_file_daemon_create (GFile *file,
		      GCancellable *cancellable,
		      GError **error)
{
  /* TODO: implement */
  return NULL;
}

static GFileOutputStream *
g_file_daemon_replace (GFile *file,
		       time_t mtime,
		       gboolean  make_backup,
		       GCancellable *cancellable,
		       GError **error)
{
  /* TODO: implement */
  return NULL;
}

static void
g_file_daemon_file_iface_init (GFileIface *iface)
{
  iface->copy = g_file_daemon_copy;
  iface->is_native = g_file_daemon_is_native;
  iface->get_path = g_file_daemon_get_path;
  iface->get_uri = g_file_daemon_get_uri;
  iface->get_parse_name = g_file_daemon_get_parse_name;
  iface->get_parent = g_file_daemon_get_parent;
  iface->get_child = g_file_daemon_get_child;
  iface->enumerate_children = g_file_daemon_enumerate_children;
  iface->get_info = g_file_daemon_get_info;
  iface->read = g_file_daemon_read;
  iface->append_to = g_file_daemon_append_to;
  iface->create = g_file_daemon_create;
  iface->replace = g_file_daemon_replace;
  iface->read_async = g_file_daemon_read_async;
}
