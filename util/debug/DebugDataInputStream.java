import java.io.*;

public class DebugDataInputStream implements DataInput
{
	private final DataInputStream input;
	
	public DebugDataInputStream(DataInputStream input)
	{
		this.input = input;
	}
	
	public DebugDataInputStream(InputStream input)
	{
		this(new DataInputStream(input));
	}
	
	public boolean readBoolean() throws IOException
	{
		return input.readBoolean();
	}
	
	public byte readByte() throws IOException
	{
		byte output = input.readByte();
		System.out.println("readByte() = " + hex(output));
		return output;
	}
	
	public char readChar() throws IOException
	{
		return input.readChar();
	}
	
	public double readDouble() throws IOException
	{
		return input.readDouble();
	}
	
	public float readFloat() throws IOException
	{
		return input.readFloat();
	}
	
	public void readFully(byte[] b) throws IOException
	{
		input.readFully(b);
	}
	
	public void readFully(byte[] b, int off, int len) throws IOException
	{
		input.readFully(b, off, len);
	}
	
	public int readInt() throws IOException
	{
		int output = input.readInt();
		System.out.println("readInt() = " + hex(output));
		return output;
	}
	
	/** @deprecated */
	public String readLine() throws IOException
	{
		return input.readLine();
	}
	
	public long readLong() throws IOException
	{
		return input.readLong();
	}
	
	public short readShort() throws IOException
	{
		short output = input.readShort();
		System.out.println("readShort() = " + hex(output));
		return output;
	}
	
	public int readUnsignedByte() throws IOException
	{
		return input.readUnsignedByte();
	}
	
	public int readUnsignedShort() throws IOException
	{
		return input.readUnsignedShort();
	}
	
	public String readUTF() throws IOException
	{
		return input.readUTF();
	}
	
	public int skipBytes(int n) throws IOException
	{
		return input.skipBytes(n);
	}
	
	public static String hex(byte b)
	{
		return hex(b, 2);
	}
	
	public static String hex(short s)
	{
		return hex(s, 4);
	}
	
	public static String hex(int i)
	{
		return hex(i, 8);
	}
	
	public static String hex(int i, int pad)
	{
		String hex = Integer.toHexString(i);
		while(hex.length() < pad)
			hex = "0" + hex;
		return "0x" + hex;
	}
}
