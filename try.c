#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __unix__
  #include <termios.h>
  #include <unistd.h>
  #include <sys/types.h>
  #include <fcntl.h>

  #include <errno.h>
//  #include <getopt.h>
  #include <sys/stat.h>
  #define UINT64 unsigned long
  #define SNPRINTF snprintf
  #define FILE_SIZE_T size_t
#endif // __unix__
#ifdef _WIN32
//  #include "getopt.h" // use custom file
  #include <windows.h>
  #define UINT64 unsigned long long
  #define SNPRINTF sprintf_s
  #define FILE_SIZE_T unsigned long long
  //#include <stdlib.h>
  #include <crtdbg.h>
#endif // _WIN32


#define STEPPER_SRC_FREQ (7372800) // 7.4 MHz

struct termios orig_serial_port_settings;
struct termios curr_serial_port_settings;

// --------------------------------------------------------------------------
void print_dump(char *data, int size)
{
  int paragraphs = size / 0x10 + (!!(size % 0x10));
  int curr_tetrade = 0;
  int curr_paragraph;
  int curr_byte;

  for (curr_paragraph = 0; curr_paragraph < paragraphs; curr_paragraph ++)
  {
    // display address
    printf("%08X | ", curr_paragraph * 0x10);

    // display hex bytes
    for (curr_tetrade = 0; curr_tetrade < 4; curr_tetrade++)
    {
      for(curr_byte = 0; curr_byte < 4; curr_byte++)
      {
        if (curr_paragraph * 0x10 + curr_tetrade * 4 + curr_byte < size)
        {
          // print actual hex value if byte offset is below array size
          printf("%02X ", data[curr_paragraph * 0x10 + curr_tetrade * 4 + curr_byte] & 0xFF);
        } else
        {
          // else print white space
          printf("   ");
        }
      }
      // print tetrade delimiter
      printf("| ");
    }

    // display ascii values
    for (curr_byte = 0; curr_byte < 0x10; curr_byte++)
    {
      if (curr_paragraph * 0x10 + curr_byte < size)
      {
        // print only readable data, otherwice put '.'
        printf("%c", ((data[curr_paragraph * 0x10 + curr_byte] < 32) || (data[curr_paragraph * 0x10 + curr_byte] > 127)) ? 
          '.' : 
          data[curr_paragraph * 0x10 + curr_byte]);
      } else
      {
        printf(" ");
      }
    }
    printf("\n");
  }
}


// ---------------------------------------------------------------------------
struct wl_cal_point_struc
{
  int step;
  double wavelength;
};

struct wl_cal_context_struc
{
  int initialized;
  struct wl_cal_point_struc *cal_data;
  int cal_data_size;
};


// ---------------------------------------------------------------------------
int get_file_size(char *filename, FILE_SIZE_T *file_length)
{
  int result;

#ifdef __unix__
  struct stat file_stat_struc;

  result = stat(filename, &file_stat_struc);
  if (result != 0)
  {
    fprintf(stderr, "**Error**: stat returned error %d, \"%s\", file \"%s\"\n", errno, strerror(errno), filename);
    return result;
  }

  *file_length = file_stat_struc.st_size;
#endif // __unix__

#ifdef _WIN32
  HANDLE hFile;

  hFile = CreateFileA((LPCSTR) filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE)
  {
    fprintf(stderr, "**Error**: unable to open file\"%s\"\n", filename);
    return -1;
  }

  if (! (result = GetFileSizeEx(hFile, (PLARGE_INTEGER)file_length)))
  {
    fprintf(stderr, "**Error**: failed to request file size, error is %ld\n", GetLastError());
  }

  if (! CloseHandle(hFile))
  {
    fprintf(stderr, "**Error**: failed to close file, error is %ld\n", GetLastError());
    return -1;
  }

  // result pending from GetFileSizeEx
  if (!result) return -1;
#endif // _WIN32

  return 0;
}


// ---------------------------------------------------------------------------
int read_file_image(char *filename, char **image, FILE_SIZE_T *image_length)
{
  int result;
  FILE *file_stream;

#ifdef _WIN32
  char error_buffer[256];
#endif // _WIN32

  result = get_file_size(filename, image_length);
  if (result) 
  {
    *image = NULL;
    *image_length = 0;
    return result;
  }

  if ((*image = (char *) malloc((size_t)(*image_length * sizeof(char)))) == NULL)
  {
    *image = NULL;
    *image_length = 0;
    fprintf(stderr, "**Error**: failed to allocate memory for file\n");
    return -2;
  } else
  {
#ifdef __unix__
    if ((file_stream = fopen(filename, "rb")) == NULL)
#endif // __unix__
#ifdef _WIN32
    if (fopen_s(&file_stream, filename, "rb"))
#endif // _WIN32
    {
#ifdef __unix__
      fprintf(stderr, "**Error**: fopen returned error %d, \"%s\"\n", errno, strerror(errno));
#endif // __unix__
#ifdef _WIN32
      strerror_s(error_buffer, sizeof(error_buffer), errno);
      fprintf(stderr, "**Error**: fopen returned error %d, \"%s\"\n", errno, error_buffer);
#endif // _WIN32
      free(*image);
      *image = NULL;
      *image_length = 0;
      return -3;
    } else
    {
      if (fread(*image, sizeof(char), (size_t)(*image_length), file_stream) != *image_length)
      {
#ifdef __unix__
        fprintf(stderr, "**Error**: fread did not read entire file, error %d, \"%s\"\n", errno, strerror(errno));
#endif // __unix__
#ifdef _WIN32
        strerror_s(error_buffer, sizeof(error_buffer), errno);
        fprintf(stderr, "**Error**: fread did not read entire file, error %d, \"%s\"\n", errno, error_buffer);
#endif // _WIN32
        fclose(file_stream);
        free(image);
        *image = NULL;
        *image_length = 0;
        return -4;
      } else
      {
        fclose(file_stream);
        return 0;
      }
    }
  }
}

// ---------------------------------------------------------------------------
int wl_cal_allocate_context(struct wl_cal_context_struc **wl_cal_context)
{
  if ( (*wl_cal_context = (struct wl_cal_context_struc *)malloc(sizeof(struct wl_cal_context_struc))) == NULL)
  {
    fprintf(stderr, "**Error**: wl_cal_allocate_context: Failed to allocate memory for context\n");
    return -1;
  }

  memset(*wl_cal_context, 0, sizeof(struct wl_cal_context_struc));
  return 0;
}

// ---------------------------------------------------------------------------
int wl_cal_free_context(struct wl_cal_context_struc **wl_cal_context)
{
  if (*wl_cal_context != NULL)
  {
    if ((*wl_cal_context)->cal_data != NULL)
    {
      free((*wl_cal_context)->cal_data);
      (*wl_cal_context)->cal_data = NULL;
    }

    free(*wl_cal_context);
    *wl_cal_context = NULL;
  }

  return 0;
}

// ---------------------------------------------------------------------------
int wl_cal_read_table_file(struct wl_cal_context_struc *wl_cal_context, char *filename)
{
  int result;
  int sorted;
  FILE_SIZE_T i;
  struct wl_cal_point_struc temp_point;

  struct
  {
    char *i_ptr;
    FILE_SIZE_T i_length;
  } image;

  struct
  {
    char **l_array;
    int l_count;
    int l_index;
  } line;

  if (wl_cal_context == NULL)
  {
    fprintf(stderr, "**Error**: wl_cal_read_table_file: No context\n");
    return -1;
  }

  memset(&line, 0, sizeof(line));

  result = read_file_image(filename, &image.i_ptr, &image.i_length);
  if (result < 0) return result;

  for (i = 0; i < image.i_length; i++)
  {
    // skip empty lines, etc
    if ((image.i_ptr[i] != '\n') && (image.i_ptr[i] != '\r'))
    {
      line.l_count ++;
      for (; i < image.i_length; i++)
      {
        if ((image.i_ptr[i] == '\n') || (image.i_ptr[i] == '\r'))
        {
          break;
        }
      }
    } else
    {
      for (i++; i < image.i_length; i++)
      {
        if ((image.i_ptr[i] != '\n') && (image.i_ptr[i] != '\r'))
        {
          i--;
          break;
        }
      }
    }
  }

  if ((line.l_array = (char **)malloc(line.l_count * sizeof(char *))) == NULL)
  {
    fprintf(stderr, "**Error**: wl_cal_read_table_file: Failed to allocate memory for line buffer\n");
    return -71;
  }

  for (i = 0; i < image.i_length; i++)
  {
    // skip empty lines, etc
    if ((image.i_ptr[i] != '\n') && (image.i_ptr[i] != '\r'))
    {
      line.l_array[line.l_index] = image.i_ptr + i;
      // remove leading spaces
      for (; i < image.i_length; i++)
      {
        if ((image.i_ptr[i] == ' ') || (image.i_ptr[i] == '\t'))
        {
          line.l_array[line.l_index] ++;
        } else
        {
          break;
        }
      }

      if ((image.i_ptr[i] != '\n') && (image.i_ptr[i] != '\r'))
      {
        line.l_index ++;
      }

      for (; i < image.i_length; i++)
      {
        if ((image.i_ptr[i] == '\n') || (image.i_ptr[i] == '\r'))
        {
          i--;
          break;
        }
      }
    } else
    {
      for (; i < image.i_length; i++)
      {
        if ((image.i_ptr[i] != '\n') && (image.i_ptr[i] != '\r'))
        {
          i--;
          break;
        } else
        {
          image.i_ptr[i] = 0;
        }
      }
    }
  }

  line.l_count = line.l_index;

  // allocate array
  if ((wl_cal_context->cal_data = (struct wl_cal_point_struc *)malloc(line.l_count * sizeof(struct wl_cal_point_struc))) == NULL)
  {
    fprintf(stderr, "**Error**: wl_cal_read_table_file: Failed to allocate memory for spectrum.\n");
    return -81;
  }

  wl_cal_context->cal_data_size = 0;

  for (i = 0; i < line.l_count; i++)
  {
    if (line.l_array[i][0] != '#') // skip commented lines
    {
#ifdef __unix__
      sscanf(line.l_array[i], "%d\t%lf", 
        &wl_cal_context->cal_data[wl_cal_context->cal_data_size].step,
        &wl_cal_context->cal_data[wl_cal_context->cal_data_size].wavelength);
#endif // __unix__
#ifdef _WIN32
      sscanf_s(line.l_array[i], "%d\t%lf", 
        &wl_cal_context->cal_data[wl_cal_context->cal_data_size].step,
        &wl_cal_context->cal_data[wl_cal_context->cal_data_size].wavelength);
#endif // _WIN32
      wl_cal_context->cal_data_size ++;
    }
  }

  // free file image
  free(image.i_ptr);
  image.i_ptr = NULL;
  image.i_length = 0;

  // free line table
  free(line.l_array);
  line.l_array = NULL;
  line.l_count = 0;
  line.l_index = 0;

  // sort
  sorted = 0;
  while (! sorted)
  {
    sorted = 1;
    for (i = 0; i < wl_cal_context->cal_data_size - 1; i++)
    {
      if (wl_cal_context->cal_data[i].step > wl_cal_context->cal_data[i+1].step)
      {
        temp_point = wl_cal_context->cal_data[i];
        wl_cal_context->cal_data[i] = wl_cal_context->cal_data[i+1];
        wl_cal_context->cal_data[i+1] = temp_point;

        sorted = 0;
      }
    }
  }

/*
  for (i = 0; i < wl_cal_context->cal_data_size; i++)
  {
    printf("%d\t%d\t%f\n", i, wl_cal_context->cal_data[i].step, wl_cal_context->cal_data[i].wavelength);
  }
*/

  wl_cal_context->initialized = 1;

  return 0;
}

// ---------------------------------------------------------------------------
int wl_cal_wl2step(struct wl_cal_context_struc *wl_cal_context, double wavelength, int *step)
{
  int i;

  if (wl_cal_context == NULL)
  {
    fprintf(stderr, "**Error**: wl_cal_wl2step: No context\n");
    return -1;
  }

  if (wl_cal_context->initialized != 1)
  {
    fprintf(stderr, "**Error**: wl_cal_wl2step: Calibration table is not initialized\n");
    return -2;
  }

  for (i = 0; i < wl_cal_context->cal_data_size - 1; i++)
  {
    if ((wl_cal_context->cal_data[i].wavelength <= wavelength) && (wl_cal_context->cal_data[i+1].wavelength >= wavelength))
    {
      *step = wl_cal_context->cal_data[i].step + (wl_cal_context->cal_data[i+1].step - wl_cal_context->cal_data[i].step) /
        (wl_cal_context->cal_data[i+1].wavelength - wl_cal_context->cal_data[i].wavelength) *
        (wavelength - wl_cal_context->cal_data[i].wavelength);

      return 0;
    }
  }

  fprintf(stderr, "**Error**: wl_cal_wl2step: Specified wavelength is out of boundaries of calibration table\n");
  return -3;
}


// ---------------------------------------------------------------------------
int rs232_write_string(int fd, char *string)
{
  return write(fd, string, strlen(string));
}

// ---------------------------------------------------------------------------
int rs232_read_buffer(int fd, char *buffer, int max_buf_length)
{
  int result;
  int i;

  for (i = 0; i < max_buf_length - 1; i++)
  {
    if (result = read(fd, &buffer[i], 1) < 0) break;

    if ((buffer[i] == '\n') || (buffer[i] == '\r')) break;
  }

  buffer[i] = 0;

  return result;
}


// ---------------------------------------------------------------------------
int rs232_open(int *dev_fd, char *dev_file)
{
  int result;

  *dev_fd = open(dev_file, O_RDWR | O_NOCTTY );
  if(*dev_fd < 0)
  {
    fprintf(stderr, "**Error**: rs232_open: Failed to open device %s, exiting\n", dev_file);
    *dev_fd = 0;
    return -1;
  }

  // read current state of serial port
  result = tcgetattr(*dev_fd, &orig_serial_port_settings);
  if (result == -1)
  {
    fprintf(stderr, "**Error**: rs232_open: Unable to get terminal settings (%s)\n", strerror(errno));
    return -2;
  }

  curr_serial_port_settings = orig_serial_port_settings;

  result = cfsetispeed(&curr_serial_port_settings, B19200);
  if (result == -1)
  {
    fprintf(stderr, "**Error**: rs232_open: Unable to set terminal input speed (%s)\n", strerror(errno));
    return -3;
  }

  result = cfsetospeed(&curr_serial_port_settings, B19200);
  if (result == -1)
  {
    fprintf(stderr, "**Error**: rs232_open: Unable to set terminal output speed (%s)\n", strerror(errno));
    return -4;
  }

  curr_serial_port_settings.c_lflag = 0;

  curr_serial_port_settings.c_oflag = 0;

  curr_serial_port_settings.c_iflag = 0;

  curr_serial_port_settings.c_cflag |= (CREAD | CLOCAL);
  curr_serial_port_settings.c_cflag &= ~(CSTOPB);
  curr_serial_port_settings.c_cflag &= ~(CSIZE);
  curr_serial_port_settings.c_cflag |= (PARENB | PARODD | CS8 );  // 8 bit, odd parity
  curr_serial_port_settings.c_cflag &= ~(CRTSCTS);

  curr_serial_port_settings.c_cc[VMIN]  = 0;
  curr_serial_port_settings.c_cc[VTIME] = 0;

  result = tcsetattr(*dev_fd, TCSANOW, &curr_serial_port_settings);
  if (result == -1)
  {
    fprintf(stderr, "**Error**: rs232_open: Unable to set terminal settings (%s)\n", strerror(errno));
    return -5;
  }

  // discard old data in rx and tx buffer
  result = tcflush(*dev_fd, TCIOFLUSH);
  if (result == -1)
  {
    fprintf(stderr, "**Error**: rs232_open: Unable to flush terminal (%s)\n", strerror(errno));
    return -6;
  }

  return 0;
}

// ---------------------------------------------------------------------------
int rs232_close(int *dev_fd)
{
  int result;

  result = tcsetattr(*dev_fd, TCSANOW, &orig_serial_port_settings);
  if (result == -1)
  {
    fprintf(stderr, "**Error**: rs232_close: unable to set terminal settings (%s)\n", strerror(errno));
    return -1;
  }

  // discard old data in rx and tx buffer
  result = tcflush(*dev_fd, TCIOFLUSH);
  if (result == -1)
  {
    fprintf(stderr, "**Error**: rs232_close: Unable to flush terminal (%s)\n", strerror(errno));
    return -2;
  }

  close(*dev_fd);
  *dev_fd = 0;

  return 0;
}


/*
// -------------------------------------------------------------------------
int stepper_init(char *dev_file_name, int *dev_fd)
{
  int error;
#ifdef SETUID
  int old_uid, old_gid;
  int uid_changed = 0;
#endif // SETUID
  struct termios old_dev_tio, new_dev_tio;

  fd_set dev_read_fds;
  struct timeval dev_tv;

  char *dev_cmd_read_request = (channel_number == 1) ? ":acq1:lmem?\n" : ":acq2:lmem?\n";

  int dev_data_index = 0;
  int dev_read_len;

#ifdef SETUID
  if ((dev_fd = open(dev_file_name, O_RDWR | O_NOCTTY)) < 0) // check if we have permissions
  {
    old_uid = getuid(); old_gid = getgid();
    if (setuid(0) || setgid(0))
    {
      fprintf(stderr, "**Error**: You have no permissions to access \"%s\"\n"
                      "           Setting UID and GID to root failed\n", dev_file_name);
      return ERR_DEVICE_OPEN;
    }

    uid_changed = 1;
    // already root:wheel
    if ((dev_fd = open(dev_file_name, O_RDWR | O_NOCTTY)) < 0)
    {
      fprintf(stderr, "**Error**: Error opening device \"%s\"\n", dev_file_name);
      setuid(old_uid); setgid(old_gid);
      return ERR_DEVICE_OPEN;
    }
  }
#else // SETUID
  if ((dev_fd = open(dev_file_name, O_RDWR | O_NOCTTY)) < 0)
  {
    fprintf(stderr, "**Error**: Error opening device \"%s\"\n", dev_file_name);
    return ERR_DEVICE_OPEN;
  }
#endif // SETUID

  tcgetattr(dev_fd, &old_dev_tio);
  bzero(&new_dev_tio, sizeof(new_dev_tio));
  new_dev_tio.c_cflag = B19200 | CS8 | CLOCAL | CREAD;
  new_dev_tio.c_iflag = IGNBRK | IXON | IXOFF;
  new_dev_tio.c_oflag = 0;
  new_dev_tio.c_lflag = 0;
  new_dev_tio.c_cc[VTIME] = 5;
  new_dev_tio.c_cc[VMIN] = 1;
  new_dev_tio.c_ispeed = 13;
  new_dev_tio.c_ospeed = 13;
  tcflush(dev_fd, TCIFLUSH);
  tcsetattr(dev_fd, TCSANOW, &new_dev_tio);

  write(dev_fd, dev_cmd_read_request, strlen(dev_cmd_read_request));

  FD_ZERO(&dev_read_fds);
  FD_SET(dev_fd, &dev_read_fds);

  while (1)
  {
    dev_tv.tv_sec = 0;
    dev_tv.tv_usec = 200000;
    error = select(dev_fd + 1, &dev_read_fds, NULL, NULL, &dev_tv);
    switch(error)
    {
       case -1:
         fprintf(stderr, "**Error**: Error reading from device: select() failed with errorcode %d\n", error);
         close(dev_fd);
         return ERR_DEVICE_READ;
       case 0:
         close(dev_fd);
         gds_buffer_length = dev_data_index;
#ifdef SETUID
         if (uid_changed)  // reset uid/gid to create files belonging to current user but not root
         {
           setuid(old_uid);
           setgid(old_gid);
         }
#endif
         return 0;
       default:
         dev_read_len = read(dev_fd, gds_buffer + dev_data_index, GDS_BUFFER_LENGTH - dev_data_index);
         dev_data_index += dev_read_len;
    }
  }
  
}
*/



// ---------------------------------------------------------------------------
int stepper_rotate(int dev_fd, int steps, int micro_step_flag, int stepper_freq)
{
  unsigned char data_buffer[5];
  unsigned char responce[1];
  int freq_word;
  int result;
  int i;

  int failure;

  for (i = 0; i < 5; i++)
  {
    data_buffer[i] = 0;
  }
//  memset(data_buffer, 0, sizeof(data_buffer));

  data_buffer[4] = (1 << 3); // -reset bit must always be equal to 1

  if (stepper_freq < 120)
  {
    freq_word = (0xFFFF - (STEPPER_SRC_FREQ) / (stepper_freq * 64));
    data_buffer[4] |= (1 << 4); // set low speed bit
  } else
  {
    freq_word = (0xFFFF - (STEPPER_SRC_FREQ) / (stepper_freq * 1));
    data_buffer[4] &= ~(1 << 4); // drop low speed bit
  }

  if (steps < 0)
  {
    data_buffer[4] |= (1 << 2); // negation sign
    steps = -steps;
  } else
  {
    data_buffer[4] &= ~(1 << 2); // positive
  }

  if (micro_step_flag)
  {
    data_buffer[4] |= (1 << 0); // microstep enable
  } else
  {
    data_buffer[4] &= ~(1 << 0); // microstep disable
  }

  data_buffer[0] = (freq_word >> 8) & 0xFF;  // first high
  data_buffer[1] = (freq_word >> 0) & 0xFF;  // then low

  data_buffer[2] = (steps >> 8) & 0xFF;  // first high
  data_buffer[3] = (steps >> 0) & 0xFF;  // then low


  data_buffer[4] = 0xF;
  // run until ready accepted message is received

  // send char by char

//  tcflush(dev_fd, TCIFLUSH);

  print_dump(data_buffer, 5);

  do
  {
    failure = 1;

    for (i = 0; i < 5; i++)
    {
      result = write(dev_fd, &data_buffer[i], 1);
      if (result < 0)
      {
        fprintf(stderr, "**Error**: stepper_rotate: write to device failed with error %d, \"%s\"\n", errno, strerror(errno));
        return errno;
      }

      usleep(10000);

      responce[0] = 0;
      result = read(dev_fd, responce, 1);
      if (result < 0)
      {
        fprintf(stderr, "**Error**: stepper_rotate: read from device failed with error %d, \"%s\"\n", errno, strerror(errno));
        return errno;
      }

//      printf("Result [%d] is %x\n", i, (int)(responce & 0xFF));
      printf("Result [%d] is %02x\n", i, responce[0]);

/*
      if (((int)(responce & 0xFF) != 0xFA)) // 
      {

        usleep(10000);

        result = read(dev_fd, responce, 1);
        if (result < 0)
        {
          fprintf(stderr, "**Error**: stepper_rotate: read from device failed with error %d, \"%s\"\n", errno, strerror(errno));
          return errno;
        }

        printf("Result 2 [%d] is %x\n", i, (int)(responce & 0xFF));
      }

*/

//      if (((int)(responce & 0xFF) != 0xFA)) // 
/*
      if (responce[0] != 0xFA) // 
      {
        failure ++;
        break;
      }
*/
    }
  } while (failure);

  usleep(100000);

  responce[0] = 0;
  result = read(dev_fd, responce, 1);
  if (result < 0)
  {
    fprintf(stderr, "**Error**: stepper_rotate: read from device failed with error %d, \"%s\"\n", errno, strerror(errno));
    return errno;
  }

  printf("Result end is %x\n", responce[0]);


  return 0;
}


// ---------------------------------------------------------------------------
int main(void)
{
  int result;
  int step;
  struct wl_cal_context_struc *wl_cal_context;

  int dev_fd;

  printf("Stepper v. 0.1 (C) S.Ambrozevich, LPI\n");
/*
  result = wl_cal_allocate_context(&wl_cal_context);
  if (result < 0) return result;

  result = wl_cal_read_table_file(wl_cal_context, "test_table.txt");
  if (result < 0) return result;

  result = wl_cal_wl2step(wl_cal_context, 700.0, &step);
  if (result < 0) return result;
  printf("Result is %d\n", step);

  result = wl_cal_free_context(&wl_cal_context);
  if (result < 0) return result;
*/

  result = rs232_open(&dev_fd, "/dev/ttyS2");
  if (result < 0) return result;

  // int stepper_rotate(int dev_fd, int steps, int micro_step_flag, int stepper_freq);
  result = stepper_rotate(dev_fd, 6400, 0, 3200);
  if (result < 0) return result;

  //usleep(1000000);
//
 // result = stepper_rotate(dev_fd, 5000, 0, 100);
 // if (result < 0) return result;
 // rs232_close(&dev_fd);



  return 0;
}



