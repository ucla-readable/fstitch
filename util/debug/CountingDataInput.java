import java.io.*;

public class CountingDataInput implements DataInput
{
	private DataInput input;
	private long offset;
	
	public CountingDataInput(DataInput input)
	{
		this.input = input;
		offset = 0;
	}
	
	public boolean readBoolean() throws IOException
	{
		offset++;
		return input.readBoolean();
	}
	
	public byte readByte() throws IOException
	{
		offset++;
		return input.readByte();
	}
	
	public char readChar() throws IOException
	{
		throw new RuntimeException("readChar not supported");
	}
	
	public double readDouble() throws IOException
	{
		offset += 8;
		return input.readDouble();
	}
	
	public float readFloat() throws IOException
	{
		offset += 4;
		return input.readFloat();
	}
	
	public void readFully(byte b[]) throws IOException
	{
		offset += b.length;
		input.readFully(b);
	}
	
	public void readFully(byte b[], int off, int len) throws IOException
	{
		offset += len;
		input.readFully(b, off, len);
	}
	
	public int readInt() throws IOException
	{
		offset += 4;
		return input.readInt();
	}
	
	public String readLine() throws IOException
	{
		throw new RuntimeException("readLine not supported");
	}
	
	public long readLong() throws IOException
	{
		offset += 8;
		return input.readLong();
	}
	
	public short readShort() throws IOException
	{
		offset += 2;
		return input.readShort();
	}
	
	public int readUnsignedByte() throws IOException
	{
		offset++;
		return input.readUnsignedByte();
	}
	
	public int readUnsignedShort() throws IOException
	{
		offset += 2;
		return input.readUnsignedShort();
	}
	
	public String readUTF() throws IOException
	{
		throw new RuntimeException("readUTF not supported");
	}
	
	public int skipBytes(int n) throws IOException
	{
		int nn = input.skipBytes(n);
		offset += nn;
		return nn;
	}
	
	public long getOffset()
	{
		return offset;
	}
}
