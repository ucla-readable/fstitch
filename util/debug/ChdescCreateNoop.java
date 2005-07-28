import java.io.DataInput;
import java.io.IOException;

class ChdescCreateNoopFactory extends ModuleOpcodeFactory
{
	public ChdescCreateNoopFactory(DataInput input)
	{
		super(input, KDB_CHDESC_CREATE_NOOP, "KDB_CHDESC_CREATE_NOOP");
		addParameter("chdesc", 4);
		addParameter("block", 4);
		addParameter("owner", 4);
	}
	
	public ChdescCreateNoop readChdescCreateNoop() throws IOException
	{
		input.readByte();
		int chdesc = input.readInt();
		input.readByte();
		int block = input.readInt();
		input.readByte();
		int owner = input.readInt();
		input.readShort();
		return new ChdescCreateNoop(chdesc, block, owner);
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescCreateNoop();
	}
}

public class ChdescCreateNoop extends Opcode
{
	public ChdescCreateNoop(int chdesc, int block, int owner)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescCreateNoopFactory(input);
	}
}
