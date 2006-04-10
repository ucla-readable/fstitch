import java.io.*;

public abstract class OpcodeFactory implements Constants
{
	protected final CountingDataInput input;
	
	protected OpcodeFactory(CountingDataInput input)
	{
		this.input = input;
	}
	
	public String readString() throws IOException
	{
		ByteArrayOutputStream string = new ByteArrayOutputStream();
		byte b = input.readByte();
		while(b != 0)
		{
			string.write(b);
			b = input.readByte();
		}
		return new BufferedReader(new InputStreamReader(new ByteArrayInputStream(string.toByteArray()))).readLine().intern();
	}
	
	public int getInputOffset()
	{
		return input.getOffset();
	}
	
	public abstract Opcode readOpcode() throws BadInputException, IOException;
}
