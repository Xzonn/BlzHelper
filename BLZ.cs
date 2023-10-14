using System;
using System.Linq;

namespace Xzonn.BlzHelper
{
  public static class BLZ
  {
    /// <summary>
    /// Decompresses the input byte array using the BLZ algorithm.
    /// </summary>
    /// <param name="input"></param>
    /// <returns></returns>
    public static byte[] Decompress(byte[] input)
    {
      var sizeDiff = BitConverter.ToInt32(input, input.Length - 4);
      var headerLength = input[input.Length - 5];
      var compressedLength = BitConverter.ToInt32(input, input.Length - 8) & 0x00ffffff;
      var inverted = input.Reverse().Skip(headerLength).Take(compressedLength - headerLength).ToArray();
      var output = new byte[input.Length + sizeDiff];
      Array.Copy(input, 0, output, 0, input.Length - compressedLength);
      int outputPos = output.Length;
      for (int i = 0; i < inverted.Length;)
      {
        byte flag = inverted[i++];
        for (byte bit = 0x80; bit > 0 && i < inverted.Length; bit >>= 1)
        {
          if ((flag & bit) == 0)
          {
            output[--outputPos] = inverted[i++];
          }
          else
          {
            byte b1 = inverted[i++];
            byte b2 = inverted[i++];
            int offset = (((b1 & 0x0f) << 8) | b2) + 3;
            int length = (b1 >> 4) + 3;

            outputPos -= length;
            while (length-- > 0)
            {
              output[outputPos + length] = output[outputPos + length + offset];
            }
          }
        }
      }
      return output;
    }

    /// <summary>
    /// Compresses the input byte array using the BLZ algorithm.
    /// </summary>
    /// <param name="input"></param>
    /// <returns></returns>
    public static byte[] Compress(byte[] input)
    {
      var outputTemp = new byte[input.Length];
      int inputPos = input.Length, outputPos = input.Length;
      while (true)
      {
        if (inputPos < 1)
        {
          outputTemp = outputTemp.Skip(outputPos).ToArray();
          int inputOffset = 0, outputOffset = 0;

          if (CheckOverwrite(input.Length, outputTemp, input.Length - outputPos, ref inputOffset, ref outputOffset))
          {
            outputTemp = input.Take(inputOffset).Concat(outputTemp.Skip(outputOffset)).ToArray();
          }
          int compressedEndAligned = outputTemp.Length + (4 - outputTemp.Length % 4) % 4;
          int finalSize = compressedEndAligned + 8;
          if (finalSize < input.Length)
          {
            byte[] output = new byte[finalSize];
            Array.Copy(outputTemp, output, outputTemp.Length);
            for (int i = outputTemp.Length; i < compressedEndAligned; i++)
            {
              output[i] = 0xFF;
            }
            var sizeDiff = input.Length - finalSize;
            var headerLength = finalSize - outputTemp.Length;
            var compressedLength = outputTemp.Length - inputOffset + headerLength;
            BitConverter.GetBytes((uint)sizeDiff).CopyTo(output, finalSize - 4);
            BitConverter.GetBytes((uint)((headerLength << 24) | (compressedLength & 0x00ffffff))).CopyTo(output, finalSize - 8);
            return output;
          }
          else
          {
            return new byte[0];
          }
        }
        if (outputPos < 1) break;

        byte flag = 0;
        int flagPos = --outputPos;
        for (int i = 0; i < 8; i++)
        {
          flag <<= 1;
          if (inputPos > 0)
          {
            int chunkSize = Math.Min(inputPos, 0x12);
            int bytesRead = Math.Min(input.Length - inputPos, 0x1002);
            int offset = 0;
            int length = FindMatched(input, inputPos - chunkSize, chunkSize, inputPos, bytesRead, ref offset);
            if (length < 3)
            {
              if (outputPos < 1) return new byte[0];

              outputPos--;
              inputPos--;
              outputTemp[outputPos] = input[inputPos];
            }
            else
            {
              if (outputPos < 2) return new byte[0];

              inputPos -= length;
              outputPos -= 2;
              outputTemp[outputPos] = (byte)((offset - 3) & 0xff);
              outputTemp[outputPos + 1] = (byte)(((length - 3) << 4) | ((offset - 3) >> 8));
              flag |= 1;
            }
          }
        }
        outputTemp[flagPos] = flag;
      }
      return new byte[0];
    }

    /// <summary>
    /// This function iterates over all the input after the chunk to find another
    /// chunk with the most input matching `chunk`. It returns the number of
    /// byte-wise matches found in the optimal chunk.
    /// </summary>
    /// <param name="uncompressed"></param>
    /// <param name="chunk">a pointer to a section of the input byte array of size `chunkSize`.</param>
    /// <param name="chunkSize"></param>
    /// <param name="remainder">a pointer to the remainder of the byte array of size `remainderSize`.</param>
    /// <param name="remainderSize"></param>
    /// <param name="offset">points to the end of the optimal chunk.</param>
    /// <returns></returns>
    static int FindMatched(byte[] uncompressed, int chunk, int chunkSize, int remainder, int remainderSize, ref int offset)
    {
      byte lastChar = uncompressed[chunk + chunkSize - 1];
      int maxMatches = 0;
      for (int i = 0; i < remainderSize; i++)
      {
        if (lastChar == uncompressed[remainder + i])
        {
          int bufferSize = Math.Min(i + 1, chunkSize);
          int numMatches = HowManyMatched(uncompressed, chunk + chunkSize - 1, remainder + i, bufferSize);
          if (numMatches > maxMatches)
          {
            offset = i + 1;
            maxMatches = numMatches;
          }
        }
      }
      return maxMatches;
    }

    /// <summary>
    /// Returns the number of consecutive input that match in both buffers, starting
    /// from the end of each buffer.
    /// </summary>
    /// <param name="uncompressed"></param>
    /// <param name="buffer1"></param>
    /// <param name="buffer2"></param>
    /// <param name="size"></param>
    /// <returns></returns>
    static int HowManyMatched(byte[] uncompressed, int buffer1, int buffer2, int size)
    {
      int i = 0;
      for (; i < size && uncompressed[buffer1] == uncompressed[buffer2]; buffer1--)
      {
        buffer2--;
        i++;
      }
      return i;
    }

    static bool CheckOverwrite(int sourcePos, byte[] compressed, int compressedPos, ref int sourceOffset, ref int compressedOffset)
    {
      while (true)
      {
        if (sourcePos < 1)
        {
          return false;
        }
        compressedPos--;
        byte flag = compressed[compressedPos];
        for (byte bit = 0x80; bit > 0 && sourcePos > 0; bit >>= 1)
        {
          if ((flag & bit) == 0)
          {
            compressedPos--;
            sourcePos--;
          }
          else
          {
            int nextCompressedSize = compressedPos - 2;
            sourcePos -= (compressed[compressedPos - 1] >> 4) + 3;
            if (sourcePos < 0)
            {
              return false;
            }
            compressedPos = nextCompressedSize;
            if (sourcePos < nextCompressedSize)
            {
              sourceOffset = sourcePos;
              compressedOffset = compressedPos;
              return true;
            }
          }
        }

      }
    }
  }
}
